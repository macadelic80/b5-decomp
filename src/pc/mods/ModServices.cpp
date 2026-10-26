// FLAG PC divergence: the game-state services of the mod host ("brn.player.v1", ...).
// No console counterpart.
//
// Every write goes through the game's own channels, the ones its debug menu already uses:
//   teleport / car change   GameStateModule's debug game-event queue (ResetPlayerDebugComponent)
//   boost / invulnerability SetBoostAction / PlayerInvulnerableAction (the race car module's arms)
//   reset on track          ResetPlayerCarOnTrackAction
//   place on track          ActiveRaceCar::RequestPlaceOnTrack
//   traffic                 the TrafficEntityModule debug fields (pc/debug/TrafficControls.h)
// Reads are copied into snapshots once per game tick; the mods read the copies from any thread.

#include "pc/mods/ModHost.h"

#if BRN_PC_MODS

#include "pc/mods/ModInternal.h"
#include "GameSource/GameState/BrnGameStateModule.h"
#include "GameSource/GameState/BrnGameStateModuleIO.h"
#include "GameSource/GameState/BrnGameEvents.h"
#include "GameSource/GameState/BrnGameActions.h"
#include "GameSource/GameState/ModeManager/BrnModeManager.h"
#include "GameSource/World/BrnWorldModule.h"
#include "GameSource/World/EntityModules/RaceCarEntityModule/BrnRaceCarEntityModule.h"
#include "GameSource/World/EntityModules/RaceCarEntityModule/BrnActiveRaceCar.h"
#include "GameSource/World/EntityModules/RaceCarEntityModule/BrnRaceCar.h"
#include "GameSource/World/EntityModules/TrafficEntityModule/BrnTrafficEntityModule.h"
#include "GameShared/GameClasses/Core/CgsID.h"
#include "GameShared/GameClasses/Module/CgsVariableEventQueue.h"
#include "GameShared/GameClasses/World/CgsWorldMap2D.h"
#include "SharedClasses/DataLists/VehicleList.h"
#include "SharedClasses/DataLists/VehicleListEntry.h"
#include "SharedClasses/DataLists/WheelList.h"
#include "SharedClasses/Trigger/BrnTriggerData.h"
#include "SharedClasses/Trigger/BrnGenericRegion.h"
#include "SharedClasses/Trigger/BrnRegion.h"
#include "SharedClasses/World/BrnWorldRegion.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string.h>
#include <vector>

namespace CgsPC { namespace Mods
{
    using BrnWorld::ActiveRaceCar;
    using BrnWorld::RaceCar;
    using BrnWorld::RaceCarEntityModule;

    // The one door into the world's private members (friend of WorldModule and of
    // TrafficEntityModule, FLAG PC divergence in both headers).
    struct WorldAccess
    {
        static RaceCarEntityModule& RaceCars(BrnWorld::WorldModule& lrWorld) { return lrWorld.mRaceCarEntityModule; }
        static BrnTraffic::TrafficEntityModule& Traffic(BrnWorld::WorldModule& lrWorld) { return lrWorld.mTrafficEntityModule; }

        static void ReadTraffic(const BrnTraffic::TrafficEntityModule& lrTraffic, BrnTrafficSettings& lrOut)
        {
            lrOut.size               = sizeof(BrnTrafficSettings);
            lrOut.turn_off           = lrTraffic.mbDEBUGTurnTrafficOff;
            lrOut.stop_moving        = lrTraffic.mbDEBUGStopTrafficMoving;
            lrOut.allow_anarchy      = lrTraffic.mbDEBUGAllowAnarchy;
            lrOut.enable_avoidance   = lrTraffic.mbDEBUGEnableAvoidance;
            lrOut.speed_multiplier   = lrTraffic.mfSpeedMultiplier;
            lrOut.density_scale      = lrTraffic.mfGameModeDensityScale;
            lrOut.big_vehicle_amount = lrTraffic.miBigVehicleAmount;
            lrOut.final_density      = lrTraffic.mfTrafficAmountScale;
        }

        // The debug menu's ranges (BrnTrafficDebugComponent::OnActivate @0x82762530).
        static void WriteTraffic(BrnTraffic::TrafficEntityModule& lrTraffic, const BrnTrafficSettings& lrIn)
        {
            lrTraffic.mbDEBUGTurnTrafficOff    = lrIn.turn_off != 0;
            lrTraffic.mbDEBUGStopTrafficMoving = lrIn.stop_moving != 0;
            lrTraffic.mbDEBUGAllowAnarchy      = lrIn.allow_anarchy != 0;
            lrTraffic.mbDEBUGEnableAvoidance   = lrIn.enable_avoidance != 0;
            lrTraffic.mfSpeedMultiplier        = std::fmin(std::fmax(lrIn.speed_multiplier, 0.0f), 50.0f);
            lrTraffic.mfGameModeDensityScale   = std::fmin(std::fmax(lrIn.density_scale, 0.0f), 16.0f);
            lrTraffic.miBigVehicleAmount       = lrIn.big_vehicle_amount < 0 ? 0 :
                                                 (lrIn.big_vehicle_amount > 100 ? 100 : lrIn.big_vehicle_amount);
        }

        // BrnTrafficDebugComponent::OnKillNearPlayer @0x82761C30.
        static void KillNearPlayer(BrnTraffic::TrafficEntityModule& lrTraffic)
        {
            if (lrTraffic.meLocalPlayerIndex != E_ACTIVE_RACE_CAR_INDEX_INVALID)
                lrTraffic.KillAllTrafficInCylinder(lrTraffic.mLocalPlayerPosition, 150.0f, 1000.0f, false);
        }
    };

    namespace
    {
        namespace GsIO = BrnGameState::GameStateModuleIO;

        // Everything a tick needs to apply a queued command.
        struct TickContext
        {
            BrnGameState::GameStateModule& mrGameState;
            BrnWorld::WorldModule&         mrWorld;
            GsIO::GameActionQueue*         mpActions;
        };
        typedef std::function<void(TickContext&)> Command;

        std::mutex                  sMutex;        // guards everything below
        std::vector<Command>        saCommands;
        BrnPlayerState              sPlayer = {};
        std::vector<BrnRaceCarInfo> saRaceCars;
        BrnTrafficSettings          sTraffic = {};
        bool                        sbTrafficValid = false;
        std::vector<BrnVehicleInfo> saVehicles;    // filled once, when the vehicle list is loaded
        std::vector<BrnLocation>    saLocations;   // filled once, when the trigger data is loaded
        bool                        sbInfiniteBoost = false;
        bool                        sbInvulnerable = false;

        void Post(Command lCommand)
        {
            std::lock_guard<std::mutex> lLock(sMutex);
            if (saCommands.size() < 256) saCommands.push_back(std::move(lCommand));
        }

        BrnVec3 ToBrn(const Vector3& lrVector) { return BrnVec3{ lrVector.x, lrVector.y, lrVector.z }; }

        Vector3 ToGame(const BrnVec3& lrVector)
        {
            Vector3 lResult;
            lResult.x = lrVector.x; lResult.y = lrVector.y; lResult.z = lrVector.z; lResult.w = 0.0f;
            return lResult;
        }

        void IdText(CgsID lId, char (&racOut)[16])
        {
            char lacText[KI_CGSID_STRING_LEN + 1] = {};
            CgsIDUnCompress(lId, lacText);
            size_t luLength = std::strlen(lacText);
            while (luLength > 0 && lacText[luLength - 1] == ' ') lacText[--luLength] = '\0';
            std::snprintf(racOut, sizeof(racOut), "%s", lacText);
        }

        template <size_t N> void Text(char (&racOut)[N], const char* lpcText)
        {
            std::snprintf(racOut, N, "%s", lpcText != nullptr ? lpcText : "");
        }

        ActiveRaceCar* PlayerCar(BrnWorld::WorldModule& lrWorld)
        {
            RaceCarEntityModule& lrRaceCars = WorldAccess::RaceCars(lrWorld);
            const EActiveRaceCarIndex leIndex = lrRaceCars.GetPlayerActiveRaceCarIndex();
            if (leIndex < 0 || leIndex >= E_ACTIVE_RACE_CAR_INDEX_COUNT) return nullptr;
            ActiveRaceCar* lpCar = lrRaceCars.GetActiveRaceCar(leIndex);
            return (lpCar != nullptr && lpCar->IsActive()) ? lpCar : nullptr;
        }

        // ---- snapshots -------------------------------------------------------------------------

        void CacheVehicles(BrnGameState::GameStateModule& lrGameState)
        {
            if (!saVehicles.empty()) return;
            const BrnResource::VehicleList* lpList = lrGameState.GetVehicleList();
            if (lpList == nullptr) return;
            const s32 liCount = lpList->GetVehicleCount();
            std::vector<BrnVehicleInfo> laVehicles;
            laVehicles.reserve(static_cast<size_t>(liCount > 0 ? liCount : 0));
            for (s32 li = 0; li < liCount; ++li)
            {
                const BrnResource::VehicleListEntry* lpEntry = lpList->GetVehicleData(li);
                if (lpEntry == nullptr) continue;
                BrnVehicleInfo lInfo = {};
                lInfo.id = lpEntry->GetId();
                lInfo.parent_id = lpEntry->GetParentId();
                IdText(lInfo.id, lInfo.id_text);
                Text(lInfo.name, lpEntry->GetName());
                Text(lInfo.default_wheel, lpEntry->GetDefaultWheelName());
                lInfo.car_type = lpEntry->GetCarType();
                lInfo.speed_stat = lpEntry->GetSpeedStat();
                lInfo.boost_stat = lpEntry->GetBoostStat();
                lInfo.strength_stat = lpEntry->GetStrengthStat();
                laVehicles.push_back(lInfo);
            }
            std::lock_guard<std::mutex> lLock(sMutex);
            saVehicles.swap(laVehicles);
        }

        // The spawn points the debug menu offers (ResetPlayerDebugComponent::OnActivate
        // @0x82382D48), widened from gas stations + junkyards to every drive-in region.
        void CacheLocations(BrnGameState::GameStateModule& lrGameState)
        {
            if (!saLocations.empty()) return;
            const BrnTrigger::TriggerData* lpTriggers = lrGameState.GetTriggerQueryManager()->GetTriggerData();
            CgsWorld::WorldMap2D* lpDistricts = lrGameState.GetDistrictMap();
            if (lpTriggers == nullptr || lpDistricts == nullptr) return;
            std::vector<BrnLocation> laLocations;
            for (int li = 0; li < lpTriggers->GetGenericRegionCount(); ++li)
            {
                const BrnTrigger::GenericRegion* lpRegion = lpTriggers->GetGenericRegion(li);
                if (lpRegion == nullptr) continue;
                const BrnTrigger::GenericRegion::Type leType = lpRegion->GetType();
                switch (leType)
                {
                case BrnTrigger::GenericRegion::E_TYPE_JUNK_YARD:
                case BrnTrigger::GenericRegion::E_TYPE_GAS_STATION:
                case BrnTrigger::GenericRegion::E_TYPE_BODY_SHOP:
                case BrnTrigger::GenericRegion::E_TYPE_PAINT_SHOP:
                case BrnTrigger::GenericRegion::E_TYPE_CAR_PARK:
                case BrnTrigger::GenericRegion::E_TYPE_TIRE_SHOP:
                case BrnTrigger::GenericRegion::E_TYPE_TUNING_SHOP:
                    break;
                default:
                    continue;
                }
                const BrnTrigger::BoxRegion* lpBox = lpRegion->GetBoxRegion();
                const Vector3 lFacing = lpBox->ComputeDirection();
                const Vector3 lCentre = lpBox->GetPosition();
                const f32 lfDepth = lpBox->GetDimensionZ();
                Vector3 lSpawn;
                lSpawn.x = lFacing.x * lfDepth + lCentre.x;
                lSpawn.y = lFacing.y * lfDepth + lCentre.y;
                lSpawn.z = lFacing.z * lfDepth + lCentre.z;
                lSpawn.w = 0.0f;

                Vector3 lSample = lCentre;
                lSample.w = 0.0f;
                const u8 luDistrict = lpDistricts->GetValue(lSample);
                const BrnWorld::EDistrict leDistrict = (luDistrict == CgsWorld::KU_INVALID_WORLD_MAP_VALUE ||
                                                        luDistrict >= BrnWorld::E_DISTRICT_COUNT)
                    ? BrnWorld::E_DISTRICT_INVALID : static_cast<BrnWorld::EDistrict>(luDistrict);
                const BrnWorld::ECounty leCounty = BrnWorld::WorldRegion::DistrictToCounty(leDistrict);

                const char* lpcType = BrnTrigger::GenericRegion::KAPC_GENERIC_REGION_TYPE_STRINGS[static_cast<s32>(leType)];
                BrnLocation lLocation = {};
                lLocation.type = static_cast<int>(leType);
                Text(lLocation.type_name, lpcType);
                std::snprintf(lLocation.label, sizeof(lLocation.label), "%s in %s, %s",
                              lpcType != nullptr ? lpcType : "?",
                              BrnWorld::WorldRegion::DistrictToString(leDistrict),
                              BrnWorld::WorldRegion::CountyToString(leCounty));
                lLocation.position = ToBrn(lSpawn);
                lLocation.direction = ToBrn(lFacing);
                laLocations.push_back(lLocation);
            }
            std::lock_guard<std::mutex> lLock(sMutex);
            saLocations.swap(laLocations);
        }

        const char* VehicleName(CgsID lId)
        {
            for (const BrnVehicleInfo& lrVehicle : saVehicles)
                if (lrVehicle.id == lId) return lrVehicle.name;
            return "";
        }

        void FillCar(const ActiveRaceCar& lrCar, BrnRaceCarInfo& lrOut)
        {
            const BrnPhysics::Vehicle::RaceCarState* lpState = lrCar.GetPhysicsState();
            lrOut.active = 1;
            lrOut.is_player = lrCar.IsPlayer();
            lrOut.position = ToBrn(lrCar.GetPosition());
            lrOut.direction = ToBrn(lrCar.GetDirection());
            lrOut.velocity = ToBrn(lrCar.GetVelocity());
            lrOut.speed_mph = lpState->mfSpeedMPH;
            lrOut.crashing = lpState->mbCrashing;
            const RaceCar* lpGlobal = lrCar.GetGlobalRaceCar();
            lrOut.car_id = lpGlobal != nullptr ? lpGlobal->GetModelId() : 0;
            IdText(lrOut.car_id, lrOut.car_id_text);
        }

        void RefreshSnapshots(BrnGameState::GameStateModule& lrGameState, BrnWorld::WorldModule& lrWorld)
        {
            CacheVehicles(lrGameState);
            CacheLocations(lrGameState);

            RaceCarEntityModule& lrRaceCars = WorldAccess::RaceCars(lrWorld);
            std::vector<BrnRaceCarInfo> laCars(E_ACTIVE_RACE_CAR_INDEX_COUNT);
            for (s32 li = 0; li < E_ACTIVE_RACE_CAR_INDEX_COUNT; ++li)
            {
                const ActiveRaceCar* lpCar = lrRaceCars.GetActiveRaceCar(static_cast<EActiveRaceCarIndex>(li));
                if (lpCar != nullptr && lpCar->IsActive()) FillCar(*lpCar, laCars[static_cast<size_t>(li)]);
            }

            BrnPlayerState lPlayer = {};
            lPlayer.size = sizeof(BrnPlayerState);
            if (const ActiveRaceCar* lpCar = PlayerCar(lrWorld))
            {
                const BrnPhysics::Vehicle::RaceCarState* lpState = lpCar->GetPhysicsState();
                lPlayer.valid = 1;
                lPlayer.position = ToBrn(lpCar->GetPosition());
                lPlayer.direction = ToBrn(lpCar->GetDirection());
                lPlayer.velocity = ToBrn(lpCar->GetVelocity());
                lPlayer.speed_mph = lpState->mfSpeedMPH;
                lPlayer.max_speed_mph = lpState->mfMaxSpeedMPH;
                lPlayer.max_boost_speed_mph = lpState->mfMaxBoostSpeedMPH;
                lPlayer.rpm = lpState->mfRPM;
                lPlayer.gear = lpState->mi8Gear;
                lPlayer.time_in_air = lpState->mfTimeInAir;
                lPlayer.time_drifting = lpState->mfTimeDrifting;
                lPlayer.time_boosting = lpState->mfTimeBoosting;
                lPlayer.time_crashing = lpState->mfTimeCrashing;
                lPlayer.crashing = lpState->mbCrashing;
                lPlayer.driveable = lpState->mbIsDriveable;
            }
            lPlayer.car_id = lrGameState.GetActivePlayerCarId();
            IdText(lPlayer.car_id, lPlayer.car_id_text);
            lPlayer.wheel_id = lrGameState.GetActivePlayerWheelId();
            lPlayer.in_game_mode = lrGameState.GetModeManager()->GetCurrentGameMode() != nullptr;
            lPlayer.game_mode_type = static_cast<int>(lrGameState.GetCurrentGameModeType());
            lPlayer.sim_paused = lrGameState.IsSimPaused(false, false);

            BrnTrafficSettings lTraffic = {};
            WorldAccess::ReadTraffic(WorldAccess::Traffic(lrWorld), lTraffic);

            std::lock_guard<std::mutex> lLock(sMutex);
            Text(lPlayer.car_name, VehicleName(lPlayer.car_id));
            sPlayer = lPlayer;
            saRaceCars.swap(laCars);
            sTraffic = lTraffic;
            sbTrafficValid = true;
        }

        // ---- held toggles ------------------------------------------------------------------------

        void ApplyHeld(TickContext& lrContext, bool lbInfiniteBoost, bool lbInvulnerable)
        {
            static bool sbBoostApplied = false;
            static u32 suBoostAge = 0;
            ActiveRaceCar* lpCar = PlayerCar(lrContext.mrWorld);
            if (lpCar == nullptr || lrContext.mpActions == nullptr) { sbBoostApplied = false; return; }
            const EActiveRaceCarIndex lePlayer = WorldAccess::RaceCars(lrContext.mrWorld).GetPlayerActiveRaceCarIndex();

            // Re-asserted every second while held: a mode change resets the boost strategy.
            if (lbInfiniteBoost != sbBoostApplied || (lbInfiniteBoost && ++suBoostAge >= 60))
            {
                GsIO::SetBoostAction lBoost = {};
                lBoost.meRaceCarIndex = lePlayer;
                lBoost.mxFlags = GsIO::SetBoostAction::KX_SET_INFINITE_BOOST_FLAG;
                lBoost.mbInfiniteBoost = lbInfiniteBoost;
                lrContext.mpActions->AddEvent(reinterpret_cast<const CgsModule::Event*>(&lBoost),
                                              GsIO::E_ACTION_SET_BOOST, static_cast<s32>(sizeof(lBoost)));
                sbBoostApplied = lbInfiniteBoost;
                suBoostAge = 0;
            }
            if (lbInvulnerable)
            {
                GsIO::PlayerInvulnerableAction lInvulnerable = {};
                lInvulnerable.mfInvulnerableTime = 0.5f;   // topped up every tick while held
                lrContext.mpActions->AddEvent(reinterpret_cast<const CgsModule::Event*>(&lInvulnerable),
                                              GsIO::E_ACTION_PLAYER_INVULNERABLE, static_cast<s32>(sizeof(lInvulnerable)));
            }
        }

        // ---- brn.player.v1 ------------------------------------------------------------------------

        int PlayerGetState(BrnPlayerState* lpOut)
        {
            if (lpOut == nullptr) return 0;
            std::lock_guard<std::mutex> lLock(sMutex);
            *lpOut = sPlayer;
            return sPlayer.valid;
        }

        void PostTeleport(TickContext& lrContext, BrnVec3 lPosition, BrnVec3 lDirection)
        {
            Vector3 lFacing = ToGame(lDirection);
            if (lFacing.x == 0.0f && lFacing.y == 0.0f && lFacing.z == 0.0f)
            {
                const ActiveRaceCar* lpCar = PlayerCar(lrContext.mrWorld);
                if (lpCar == nullptr) return;
                lFacing = lpCar->GetDirection();
                lFacing.w = 0.0f;
            }
            GsIO::TeleportPlayerCarEvent lEvent = {};
            lEvent.mPosition = ToGame(lPosition);
            lEvent.mDirection = lFacing;
            lrContext.mrGameState.GetDebugGameEventQueue()->AddEvent(
                reinterpret_cast<const CgsModule::Event*>(&lEvent), GsIO::E_EVENT_TELEPORT_PLAYER_CAR,
                static_cast<s32>(sizeof(lEvent)));
        }

        void PlayerTeleport(BrnVec3 lPosition, BrnVec3 lDirection)
        {
            Post([lPosition, lDirection](TickContext& lrContext) { PostTeleport(lrContext, lPosition, lDirection); });
        }

        void PlayerResetOnTrack(float lfSpeedMph)
        {
            Post([lfSpeedMph](TickContext& lrContext)
            {
                if (lrContext.mpActions == nullptr || PlayerCar(lrContext.mrWorld) == nullptr) return;
                GsIO::ResetPlayerCarOnTrackAction lAction = {};
                lAction.mfSpeed = lfSpeedMph * 0.44704f;   // the record is m/s
                lrContext.mpActions->AddEvent(reinterpret_cast<const CgsModule::Event*>(&lAction),
                                              GsIO::E_ACTION_RESET_PLAYER_CAR_ON_TRACK, static_cast<s32>(sizeof(lAction)));
            });
        }

        void PlayerPlaceOnTrack(BrnVec3 lPosition, BrnVec3 lDirection, float lfSpeedMph)
        {
            Post([lPosition, lDirection, lfSpeedMph](TickContext& lrContext)
            {
                ActiveRaceCar* lpCar = PlayerCar(lrContext.mrWorld);
                if (lpCar == nullptr) return;
                Vector3 lFacing = ToGame(lDirection);
                if (lFacing.x == 0.0f && lFacing.y == 0.0f && lFacing.z == 0.0f)
                {
                    lFacing = lpCar->GetDirection();
                    lFacing.w = 0.0f;
                }
                lpCar->RequestPlaceOnTrack(ToGame(lPosition), lFacing, std::fabs(lfSpeedMph) * 0.44704f);
            });
        }

        void PlayerSetInfiniteBoost(int liEnabled)
        {
            std::lock_guard<std::mutex> lLock(sMutex);
            sbInfiniteBoost = liEnabled != 0;
        }

        void PlayerSetInvulnerable(int liEnabled)
        {
            std::lock_guard<std::mutex> lLock(sMutex);
            sbInvulnerable = liEnabled != 0;
        }

        // ResetPlayerDebugComponent::ChangeCar @0x82382B20 + its default-wheel lookup
        // (OnChangeCarSelection @0x82382E50).
        int PlayerChangeCar(uint64_t luCarId, uint64_t luWheelId)
        {
            {
                std::lock_guard<std::mutex> lLock(sMutex);
                if (sPlayer.in_game_mode) return 0;
            }
            Post([luCarId, luWheelId](TickContext& lrContext)
            {
                BrnGameState::GameStateModule& lrGameState = lrContext.mrGameState;
                if (lrGameState.GetModeManager()->GetCurrentGameMode() != nullptr) return;
                const BrnResource::VehicleList* lpVehicles = lrGameState.GetVehicleList();
                const BrnResource::WheelList* lpWheels = lrGameState.GetWheelList();
                if (lpVehicles == nullptr || lpWheels == nullptr) return;
                const s32 liCar = lpVehicles->GetVehicleIndex(luCarId);
                if (liCar < 0) return;
                CgsID lWheel = luWheelId;
                if (lWheel == 0)
                {
                    const char* lpcDefault = lpVehicles->GetVehicleData(liCar)->GetDefaultWheelName();
                    for (s32 li = 0; lpcDefault != nullptr && li < lpWheels->GetWheelCount(); ++li)
                        if (::_stricmp(lpWheels->GetWheelData(li)->macName, lpcDefault) == 0)
                        {
                            lWheel = lpWheels->GetWheelData(li)->mID;
                            break;
                        }
                }
                GsIO::ChangePlayerCarEvent lEvent = {};
                lEvent.mCarModelId = luCarId;
                lEvent.mWheelModelId = lWheel;
                lEvent.mbResetPlayerCamera = true;
                lEvent.mbKeepResetSection = true;
                lrGameState.GetDebugGameEventQueue()->AddEvent(
                    reinterpret_cast<const CgsModule::Event*>(&lEvent), GsIO::E_EVENT_CHANGE_PLAYER_CAR,
                    static_cast<s32>(sizeof(lEvent)));
            });
            return 1;
        }

        const BrnPlayerV1 KPLAYER_V1 = {
            sizeof(BrnPlayerV1), PlayerGetState, PlayerTeleport, PlayerResetOnTrack, PlayerPlaceOnTrack,
            PlayerSetInfiniteBoost, PlayerSetInvulnerable, PlayerChangeCar,
        };

        // ---- brn.vehicles.v1 / brn.locations.v1 -------------------------------------------------

        int VehiclesCount() { std::lock_guard<std::mutex> lLock(sMutex); return static_cast<int>(saVehicles.size()); }

        int VehiclesGet(int liIndex, BrnVehicleInfo* lpOut)
        {
            std::lock_guard<std::mutex> lLock(sMutex);
            if (lpOut == nullptr || liIndex < 0 || static_cast<size_t>(liIndex) >= saVehicles.size()) return 0;
            *lpOut = saVehicles[static_cast<size_t>(liIndex)];
            return 1;
        }

        const BrnVehiclesV1 KVEHICLES_V1 = { sizeof(BrnVehiclesV1), VehiclesCount, VehiclesGet };

        int LocationsCount() { std::lock_guard<std::mutex> lLock(sMutex); return static_cast<int>(saLocations.size()); }

        int LocationsGet(int liIndex, BrnLocation* lpOut)
        {
            std::lock_guard<std::mutex> lLock(sMutex);
            if (lpOut == nullptr || liIndex < 0 || static_cast<size_t>(liIndex) >= saLocations.size()) return 0;
            *lpOut = saLocations[static_cast<size_t>(liIndex)];
            return 1;
        }

        const BrnLocationsV1 KLOCATIONS_V1 = { sizeof(BrnLocationsV1), LocationsCount, LocationsGet };

        // ---- brn.racecars.v1 ----------------------------------------------------------------------

        int RaceCarsCount() { return E_ACTIVE_RACE_CAR_INDEX_COUNT; }

        int RaceCarsGet(int liIndex, BrnRaceCarInfo* lpOut)
        {
            std::lock_guard<std::mutex> lLock(sMutex);
            if (lpOut == nullptr || liIndex < 0 || static_cast<size_t>(liIndex) >= saRaceCars.size()) return 0;
            *lpOut = saRaceCars[static_cast<size_t>(liIndex)];
            return lpOut->active;
        }

        void RaceCarsRemoveAllRivals()
        {
            Post([](TickContext& lrContext)
            {
                if (lrContext.mpActions == nullptr) return;
                GsIO::GameAction<GsIO::E_ACTION_REMOVE_ALL_RIVALS> lAction;
                lrContext.mpActions->AddEvent(reinterpret_cast<const CgsModule::Event*>(&lAction),
                                              GsIO::E_ACTION_REMOVE_ALL_RIVALS, 1);   // ProgressionManager posts 1 byte
            });
        }

        const BrnRaceCarsV1 KRACECARS_V1 = { sizeof(BrnRaceCarsV1), RaceCarsCount, RaceCarsGet, RaceCarsRemoveAllRivals };

        // ---- brn.traffic.v1 -----------------------------------------------------------------------

        int TrafficGet(BrnTrafficSettings* lpOut)
        {
            std::lock_guard<std::mutex> lLock(sMutex);
            if (lpOut == nullptr) return 0;
            *lpOut = sTraffic;
            return sbTrafficValid;
        }

        void TrafficSet(const BrnTrafficSettings* lpIn)
        {
            if (lpIn == nullptr) return;
            const BrnTrafficSettings lSettings = *lpIn;
            Post([lSettings](TickContext& lrContext)
            {
                WorldAccess::WriteTraffic(WorldAccess::Traffic(lrContext.mrWorld), lSettings);
            });
        }

        void TrafficKillNearPlayer()
        {
            Post([](TickContext& lrContext) { WorldAccess::KillNearPlayer(WorldAccess::Traffic(lrContext.mrWorld)); });
        }

        const BrnTrafficV1 KTRAFFIC_V1 = { sizeof(BrnTrafficV1), TrafficGet, TrafficSet, TrafficKillNearPlayer };
    }

    const void* FindGameService(const char* lpcName)
    {
        if (std::strcmp(lpcName, "brn.player.v1") == 0)    return &KPLAYER_V1;
        if (std::strcmp(lpcName, "brn.vehicles.v1") == 0)  return &KVEHICLES_V1;
        if (std::strcmp(lpcName, "brn.locations.v1") == 0) return &KLOCATIONS_V1;
        if (std::strcmp(lpcName, "brn.racecars.v1") == 0)  return &KRACECARS_V1;
        if (std::strcmp(lpcName, "brn.traffic.v1") == 0)   return &KTRAFFIC_V1;
        return FindDebugVarsService(lpcName);
    }

    void TickGameServices(BrnGameState::GameStateModule& lrGameState, BrnWorld::WorldModule& lrWorld,
                          BrnGameState::GameStateModuleIO::OutputBuffer* lpGameStateOutput)
    {
        std::vector<Command> laCommands;
        bool lbInfiniteBoost, lbInvulnerable;
        {
            std::lock_guard<std::mutex> lLock(sMutex);
            laCommands.swap(saCommands);
            lbInfiniteBoost = sbInfiniteBoost;
            lbInvulnerable = sbInvulnerable;
        }
        // PreWorldUpdate has released its write lock; take it again, as any producer would, for
        // the actions the mods add to this frame's queue (CheckGameActions reads it next).
        if (lpGameStateOutput != nullptr) lpGameStateOutput->LockForWrite();
        TickContext lContext = { lrGameState, lrWorld,
                                 lpGameStateOutput != nullptr ? lpGameStateOutput->GetGameActionQueue() : nullptr };
        for (Command& lrCommand : laCommands) lrCommand(lContext);
        ApplyHeld(lContext, lbInfiniteBoost, lbInvulnerable);
        if (lpGameStateOutput != nullptr) lpGameStateOutput->UnlockForWrite();
        RefreshSnapshots(lrGameState, lrWorld);
        TickDebugVars();
    }
} }

#endif // BRN_PC_MODS
