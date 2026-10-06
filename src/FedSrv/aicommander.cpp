/*-------------------------------------------------------------------------
 * aicommander.cpp
 *
 * An AI commander for a side that has no human commander.
 *
 * Every few seconds the commander of each AI side looks over its side and:
 *  - sends idle miners to the nearest He3 rock away from enemy stations and
 *    raiders, and pulls miners back to a station while raiders are about,
 *  - sends each new constructor to the best free rock for its station,
 *  - spends the side's money: two miners, research from its share of income,
 *    a few defenders, combat ships from their share, while the rest is saved
 *    for more miners and stations (stations first once it has half as many
 *    miners as it may have), and whatever is left over goes on research and
 *    combat ships; research and stations that lead to better combat ships
 *    come first,
 *  - keeps idle combat ships at home spread out between its stations and the
 *    alephs toward the enemy, rather than stacked on the launch bays,
 *  - sends combat ships after enemy ships in its own sectors and armed ones
 *    where its miners and constructors work, calling attackers back when a
 *    raid is too big, and, after the first few minutes, in waves against the
 *    nearest enemy station once enough of them are idle, keeping a few
 *    fighters home on guard. Drones won't attack a station their side hasn't
 *    seen, so until then a wave flies to a waypoint just outside it,
 *  - when raiders outnumber everything it has, keeps its combat ships docked
 *    (new ones too) instead of sending them out one at a time to be picked
 *    off, and sends them all out together once there are enough.
 *
 * The drones of an AI side may fly to rocks and alephs their side hasn't seen
 * yet, as if it had scouted them (see FedSrvSiteBase::GetCluster); in the
 * stock game a drone sent to one just sits where it is.
 *
 * Combat ships are drones (c_ptWingman) flown by the normal drone AI, on the
 * best hull the side's stations can build (the dearest; of hulls that cost the
 * same, the toughest), so better tech gives better ships.
 * Their number is limited only by money and the AIMaxShips cap.
 *
 * Turning it on: AIvsAI<n> = 1 in the server key makes every side of default
 * game <n> an AI side; AISides<n> (a bit mask, bit 0 for the first side)
 * picks the AI sides instead, for humans against the AI. The tuning values
 * below are DWORDs in the same key, HKCU first, then HKLM:
 *
 *  AIThinkSeconds     2     seconds between decisions
 *  AIMaxMiners        8     miners to keep
 *  AIMaxConstructors  2     constructors in flight at once
 *  AIMaxStations      6     stations to own, counting ones on the way
 *  AIMaxShips         100   combat ships per side (at most 200)
 *  AIMaxResearch      2     research projects at once
 *  AIAttackGroupSize  6     least ships in a wave; half as many more stay home on guard
 *  AIShipPercent      30    share of income set aside for combat ships
 *  AIResearchPercent  25    share of income set aside for research
 *  AIBomberPercent    25    share of combat ships that are bombers
 *  AIMinShipCost      250   least a combat ship costs
 *  AIShipCostPercent  100   combat ship cost, percent of hull plus parts
 *  AIShootSkill       80    combat pilot skills, percent
 *  AIMoveSkill        80
 *  AIBravery          80
 *  AIChat             2     0 silent, 1 team chat, 2 chat to everyone
 *  AIGameMinutes      60    match time limit (most kills wins), 0 for none
 *  AIAutoRestart      0     1 starts a new match when one ends
 *  AIStartSeconds     15    countdown before the match starts
 *  AIFirstAttackMinutes 5   no waves before this, so both sides can build up; ships still defend
 *  AIInitialMiners    2     miners each side starts with
 *  AIShowMap          1     1 shows every side the whole map, so drones can find the way
 *  AILogFile                (string) defaults to AICommander.log next to AllSrv.exe
 *-----------------------------------------------------------------------*/
#include "pch.h"
#include "aicommander.h"
#include <stdarg.h>
#include <stdio.h>
#include <share.h>
#include <float.h>
#include <limits.h>

/*-------------------------------------------------------------------------
 * Options and log
 */
static AIOptions    s_aiOptions;
static bool         s_bAIOptionsLoaded = false;

static bool AIReadValue(HKEY hkey, const char* pszName, DWORD* pdw, char* psz, DWORD cch)
{
    if (hkey == NULL)
        return false;

    char    bfr[MAX_PATH + 2];
    DWORD   cb = MAX_PATH;
    DWORD   type;
    if (ERROR_SUCCESS != RegQueryValueExA(hkey, pszName, NULL, &type, (BYTE*)bfr, &cb))
        return false;

    if ((type == REG_DWORD) && (cb == sizeof(DWORD)))
    {
        if (pdw == NULL)
            return false;
        *pdw = *(DWORD*)bfr;
        return true;
    }

    if ((type == REG_SZ) || (type == REG_EXPAND_SZ))
    {
        bfr[cb] = '\0';
        if (pdw)
            *pdw = strtoul(bfr, NULL, 10);
        else
            strncpy_s(psz, cch, bfr, _TRUNCATE);
        return true;
    }

    return false;
}

static DWORD AIReadDWORD(HKEY hkUser, HKEY hkMachine, const char* pszName, DWORD dwDefault)
{
    DWORD   dw;
    if (AIReadValue(hkUser, pszName, &dw, NULL, 0) || AIReadValue(hkMachine, pszName, &dw, NULL, 0))
        return dw;
    return dwDefault;
}

const AIOptions& AIGetOptions(bool bReload)
{
    if (s_bAIOptionsLoaded && !bReload)
        return s_aiOptions;
    s_bAIOptionsLoaded = true;

    HKEY    hkUser = NULL;
    HKEY    hkMachine = NULL;
    if (ERROR_SUCCESS != RegOpenKeyExA(HKEY_CURRENT_USER, HKLM_FedSrv, 0, KEY_READ, &hkUser))
        hkUser = NULL;
    if (ERROR_SUCCESS != RegOpenKeyExA(HKEY_LOCAL_MACHINE, HKLM_FedSrv, 0, KEY_READ, &hkMachine))
        hkMachine = NULL;

    AIOptions&  o = s_aiOptions;
    o.thinkSeconds      = AIReadDWORD(hkUser, hkMachine, "AIThinkSeconds",     2);
    o.maxMiners         = AIReadDWORD(hkUser, hkMachine, "AIMaxMiners",        8);
    o.maxConstructors   = AIReadDWORD(hkUser, hkMachine, "AIMaxConstructors",  2);
    o.maxStations       = AIReadDWORD(hkUser, hkMachine, "AIMaxStations",      6);
    o.maxShips          = AIReadDWORD(hkUser, hkMachine, "AIMaxShips",         100);
    o.maxResearch       = AIReadDWORD(hkUser, hkMachine, "AIMaxResearch",      2);
    o.attackGroupSize   = AIReadDWORD(hkUser, hkMachine, "AIAttackGroupSize",  6);
    o.shipPercent       = AIReadDWORD(hkUser, hkMachine, "AIShipPercent",      30);
    o.researchPercent   = AIReadDWORD(hkUser, hkMachine, "AIResearchPercent",  25);
    o.bomberPercent     = AIReadDWORD(hkUser, hkMachine, "AIBomberPercent",    25);
    o.minShipCost       = AIReadDWORD(hkUser, hkMachine, "AIMinShipCost",      250);
    o.shipCostPercent   = AIReadDWORD(hkUser, hkMachine, "AIShipCostPercent",  100);
    o.shootSkill        = AIReadDWORD(hkUser, hkMachine, "AIShootSkill",       80);
    o.moveSkill         = AIReadDWORD(hkUser, hkMachine, "AIMoveSkill",        80);
    o.bravery           = AIReadDWORD(hkUser, hkMachine, "AIBravery",          80);
    o.chat              = AIReadDWORD(hkUser, hkMachine, "AIChat",             2);
    o.gameMinutes       = AIReadDWORD(hkUser, hkMachine, "AIGameMinutes",      60);
    o.autoRestart       = AIReadDWORD(hkUser, hkMachine, "AIAutoRestart",      0);
    o.startSeconds      = AIReadDWORD(hkUser, hkMachine, "AIStartSeconds",     15);
    o.firstAttackMinutes = AIReadDWORD(hkUser, hkMachine, "AIFirstAttackMinutes", 5);
    o.initialMiners     = AIReadDWORD(hkUser, hkMachine, "AIInitialMiners",    2);
    o.showMap           = AIReadDWORD(hkUser, hkMachine, "AIShowMap",          1);

    // Keep the values where the engine can cope: every ship takes one of
    // c_cShipsMax slots, and the ship code waits forever for a free one.
    if (o.thinkSeconds < 1)
        o.thinkSeconds = 1;
    if (o.maxShips > 200)
        o.maxShips = 200;
    if (o.attackGroupSize < 1)
        o.attackGroupSize = 1;
    if (o.shipPercent > 100)
        o.shipPercent = 100;
    if (o.researchPercent > 100 - o.shipPercent)
        o.researchPercent = 100 - o.shipPercent;
    if (o.bomberPercent > 100)
        o.bomberPercent = 100;
    if (o.shootSkill > 100)
        o.shootSkill = 100;
    if (o.moveSkill > 100)
        o.moveSkill = 100;
    if (o.bravery > 100)
        o.bravery = 100;
    if (o.initialMiners > 10)
        o.initialMiners = 10;

    if (!AIReadValue(hkUser, "AILogFile", NULL, o.logFile, MAX_PATH) &&
        !AIReadValue(hkMachine, "AILogFile", NULL, o.logFile, MAX_PATH))
        o.logFile[0] = '\0';
    if (o.logFile[0] == '\0')
    {
        DWORD   cch = GetModuleFileNameA(NULL, o.logFile, MAX_PATH);
        char*   psz = ((cch > 0) && (cch < MAX_PATH)) ? strrchr(o.logFile, '\\') : NULL;
        if (psz)
            psz[1] = '\0';
        else
            o.logFile[0] = '\0';
        strcat_s(o.logFile, MAX_PATH, "AICommander.log");
    }

    if (hkUser)
        RegCloseKey(hkUser);
    if (hkMachine)
        RegCloseKey(hkMachine);

    return o;
}

void AILog(const char* format, ...)
{
    const AIOptions&    o = AIGetOptions();

    char        bfr[1600];
    SYSTEMTIME  st;
    GetLocalTime(&st);
    int n = sprintf_s(bfr, sizeof(bfr), "%04d-%02d-%02d %02d:%02d:%02d  ",
                      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    if (n < 0)
        n = 0;

    va_list va;
    va_start(va, format);
    _vsnprintf_s(bfr + n, sizeof(bfr) - n, _TRUNCATE, format, va);
    va_end(va);

    // Shared, so the log can be watched while the server writes it.
    FILE*   pfile = _fsopen(o.logFile, "a", _SH_DENYNO);
    if (pfile)
    {
        fprintf(pfile, "%s\n", bfr);
        fclose(pfile);
    }

    // And to the server's window, without the date, so the play-by-play shows there too.
    printf("%s\n", bfr + ((n >= 11) ? 11 : 0));
    fflush(stdout);
}

void AIApplyMissionParams(MissionParams* pmp)
{
    const AIOptions&    o = AIGetOptions(true);

    pmp->bAllowEmptyTeams       = true;     // AI sides have no players
    pmp->bAllowDevelopments     = true;     // the AI researches
    pmp->bInvulnerableStations  = false;    // so a side can be beaten
    pmp->bAutoStart             = true;
    pmp->bAutoRestart           = (o.autoRestart != 0);
    pmp->fStartCountdown        = (float)o.startSeconds;
    pmp->fRestartCountdown      = (float)o.startSeconds;
    pmp->dtGameLength           = 60.0f * (float)o.gameMinutes;
    pmp->iGoalConquestPercentage = 100;
    pmp->nMaxDronesPerTeam      = 10;       // the most MissionParams::Invalid allows
    pmp->nInitialMinersPerTeam  = (char)o.initialMiners;
    pmp->bShowMap               = (o.showMap != 0);    // drones only fly through alephs their side knows

    AILog("AI match settings: %d sides, %d minute limit, %d second countdown, %d starting miners, first attacks after %d minutes, restart %s, map %s",
          pmp->nTeams, o.gameMinutes, o.startSeconds, o.initialMiners, o.firstAttackMinutes, pmp->bAutoRestart ? "on" : "off",
          pmp->bShowMap ? "revealed" : "hidden");
}

/*-------------------------------------------------------------------------
 * The AI sides for default game iGame: AIvsAI<n> = 1 for every side, or
 * AISides<n> as a bit mask (bit 0 = the first side) for humans against the AI.
 */
unsigned AIGetGameSides(HKEY hKey, int iGame, int cTeams)
{
    if ((cTeams <= 0) || (cTeams > c_cSidesMax))
        cTeams = c_cSidesMax;
    unsigned    maskAll = (1u << cTeams) - 1;

    HKEY        hkUser = NULL;
    RegOpenKeyExA(HKEY_CURRENT_USER, HKLM_FedSrv, 0, KEY_READ, &hkUser);

    char        szName[32];
    DWORD       dw = 0;
    unsigned    mask = 0;

    sprintf_s(szName, "AIvsAI%d", iGame);
    if ((AIReadValue(hkUser, szName, &dw, NULL, 0) || AIReadValue(hKey, szName, &dw, NULL, 0)) && dw)
        mask = maskAll;
    else
    {
        sprintf_s(szName, "AISides%d", iGame);
        if (AIReadValue(hkUser, szName, &dw, NULL, 0) || AIReadValue(hKey, szName, &dw, NULL, 0))
            mask = dw & maskAll;
    }

    if (hkUser)
        RegCloseKey(hkUser);

    return mask;
}

/*-------------------------------------------------------------------------
 * What a commander knows about the game at one moment
 */
struct AISurvey
{
    AISurvey()
      : cMiners(0), cBuilders(0), cCombat(0), cBombers(0), cStations(0),
        cMinersBuilding(0), cStationsBuilding(0), cResearching(0), cShipsInMission(0)
    {
        ttbmHave.ClearAll();
        ttbmReach.ClearAll();
        ttbmWanted.ClearAll();
    }

    int                         cMiners;            // our miners, docked or not
    int                         cBuilders;          // our constructors on their way
    int                         cCombat;            // our combat ships
    int                         cBombers;           // ... of which bombers
    int                         cStations;
    int                         cMinersBuilding;    // miners bought and being built
    int                         cStationsBuilding;  // constructors bought and being built
    int                         cResearching;       // research projects under way
    int                         cShipsInMission;    // every ship in the game

    std::vector<IshipIGC*>      miners;             // ours, in space
    std::vector<IshipIGC*>      minersDocked;       // ours, sitting in a station
    std::vector<IshipIGC*>      builders;           // ours, in space
    std::vector<IshipIGC*>      combat;             // ours, in space
    std::vector<IshipIGC*>      combatDocked;       // ours, sitting in a station
    std::vector<IshipIGC*>      enemies;            // enemy ships our side can see, no lifepods
    std::vector<IstationIGC*>   ourStations;
    std::vector<IstationIGC*>   enemyStations;
    std::set<IclusterIGC*>      ourClusters;        // sectors with our stations
    std::set<IclusterIGC*>      enemyClusters;      // sectors with enemy stations
    std::set<IclusterIGC*>      threatClusters;     // sectors with armed enemy ships, now or in the last 45 seconds
    std::set<IclusterIGC*>      armedClusters;      // sectors with armed enemy ships now
    std::set<ImodelIGC*>        rocksTaken;         // rocks our constructors are headed for
    std::map<ImodelIGC*, int>   minersOnRock;
    std::map<const IstationTypeIGC*, int>   typeCount;  // our stations, built or coming, by type
    std::map<IclusterIGC*, int> jumps;              // jumps from the nearest sector with our station
    TechTreeBitMask             ttbmHave;           // the side's techs plus our stations' local ones
    TechTreeBitMask             ttbmReach;          // what research and stations could get us
    TechTreeBitMask             ttbmWanted;         // techs on the way to better combat ships
};

static int AIJumps(AISurvey& s, IclusterIGC* pcluster)
{
    std::map<IclusterIGC*, int>::iterator   it = s.jumps.find(pcluster);
    return (it == s.jumps.end()) ? 1000 : it->second;
}

static bool AIIsBomber(const IhullTypeIGC* pht)
{
    return (pht != NULL) && pht->HasCapability(c_habmThreatToStation);
}

static bool AIIsCombatHull(const IhullTypeIGC* pht)
{
    if (pht->HasCapability(c_habmLifepod) || pht->HasCapability(c_habmMiner) ||
        pht->HasCapability(c_habmBuilder) || pht->HasCapability(c_habmCarrier) ||
        (pht->GetGroupID() < 0))        // not for sale
        return false;

    return (pht->GetMaxFixedWeapons() > 0) || AIIsBomber(pht);
}

// How tough a hull is, to choose between ones that cost the same: hit points times guns.
static float AIHullStrength(const IhullTypeIGC* pht)
{
    return (float)pht->GetHitPoints() * (float)(1 + pht->GetMaxWeapons());
}

static bool AIIsLifepod(IshipIGC* pship)
{
    const IhullTypeIGC* pht = pship->GetBaseHullType();
    return (pht == NULL) || pht->HasCapability(c_habmLifepod);
}

// A ship that can hurt our miners: a combat drone or a player, in a hull with guns or bombs.
static bool AIIsArmed(IshipIGC* pship)
{
    PilotType   pt = pship->GetPilotType();
    if ((pt != c_ptWingman) && (pt < c_ptPlayer))
        return false;

    const IhullTypeIGC* pht = pship->GetBaseHullType();
    return (pht != NULL) && !pht->HasCapability(c_habmLifepod) &&
           ((pht->GetMaxWeapons() > 0) || AIIsBomber(pht));
}

// Where a miner hides from raiders: the nearest of our stations it can land at, in its
// own sector if there is one, else the fewest jumps away, past raiders only if it must.
static IstationIGC* AIRefuge(AISurvey& s, IshipIGC* pship)
{
    std::map<IclusterIGC*, int> jumps;
    std::vector<IclusterIGC*>   queue(1, pship->GetCluster());
    jumps[pship->GetCluster()] = 0;
    for (size_t i = 0; i < queue.size(); i++)
    {
        int j = jumps[queue[i]] + 1;
        for (WarpLinkIGC* l = queue[i]->GetWarps()->first(); l; l = l->next())
        {
            // Only through alephs our side knows of: drones can't find a way through the rest.
            IwarpIGC*       pwarpDestination = l->data()->SeenBySide(pship->GetSide()) ? l->data()->GetDestination() : NULL;
            IclusterIGC*    pcluster = pwarpDestination ? pwarpDestination->GetCluster() : NULL;
            if (pcluster && (jumps.find(pcluster) == jumps.end()))
            {
                jumps[pcluster] = j;
                queue.push_back(pcluster);
            }
        }
    }

    IstationIGC*    pbest = NULL;
    float           scoreBest = FLT_MAX;
    for (size_t i = 0; i < s.ourStations.size(); i++)
    {
        IstationIGC*    pstation = s.ourStations[i];
        std::map<IclusterIGC*, int>::iterator   it = jumps.find(pstation->GetCluster());
        if (!pstation->GetStationType()->HasCapability(c_sabmLand) || (it == jumps.end()))
            continue;

        float   score = (it->second == 0)
                        ? (pstation->GetPosition() - pship->GetPosition()).Length()
                        : 1.0e5f * (float)it->second;
        if ((it->second > 0) && (s.threatClusters.find(pstation->GetCluster()) != s.threatClusters.end()))
            score += 1.0e7f;
        if (score < scoreBest)
        {
            scoreBest = score;
            pbest = pstation;
        }
    }
    return pbest;
}

// The intruder a defender should take on: armed ones before the rest, then the
// nearest, sparing ones that already have ours on them.
static IshipIGC* AIPickIntruder(AISurvey& s, IshipIGC* pship, std::vector<IshipIGC*>& intruders,
                                std::map<ImodelIGC*, int>& onIt, float& scoreBest)
{
    // A docked ship is where its station is.
    IstationIGC*    pstation = pship->GetStation();
    IclusterIGC*    pcluster = pstation ? pstation->GetCluster() : pship->GetCluster();
    Vector          position = pstation ? pstation->GetPosition() : pship->GetPosition();

    IshipIGC*   pbest = NULL;
    scoreBest = FLT_MAX;
    for (size_t i = 0; i < intruders.size(); i++)
    {
        IshipIGC*   ptarget = intruders[i];
        float       score = (ptarget->GetCluster() == pcluster)
                            ? (ptarget->GetPosition() - position).Length()
                            : 1.0e5f * (float)(1 + AIJumps(s, ptarget->GetCluster()));
        if (!AIIsArmed(ptarget))
            score += 1.0e7f;
        std::map<ImodelIGC*, int>::iterator it = onIt.find(ptarget);
        if (it != onIt.end())
            score += 3000.0f * (float)it->second;
        if (score < scoreBest)
        {
            scoreBest = score;
            pbest = ptarget;
        }
    }
    return pbest;
}

// What the AI pays for a combat ship: the hull and its standard loadout.
static Money AIShipCost(const IhullTypeIGC* pht)
{
    const AIOptions&    o = AIGetOptions();

    Money   cost = pht->GetPrice();
    for (PartTypeLinkIGC* l = pht->GetPreferredPartTypes()->first(); l; l = l->next())
        cost += l->data()->GetPrice();

    cost = (Money)(((__int64)cost * o.shipCostPercent) / 100);
    if (cost < (Money)o.minShipCost)
        cost = (Money)o.minShipCost;

    return cost;
}

// CreateDrone mounts a hull's whole standard loadout, tech or no tech, and a ship
// docking with a part its side has no tech for hands the side that tech for free.
// So refit the way a human's default loadout is bought: only what the station sells.
// pszLeftOff gets the names of the standard parts left off.
static void AIRefit(IshipIGC* pship, IstationIGC* pstation, char* pszLeftOff, size_t cbLeftOff)
{
    pszLeftOff[0] = '\0';
    {
        const PartListIGC*  pparts = pship->GetParts();
        PartLinkIGC*        ppl;
        while ((ppl = pparts->first()) != NULL)
            ppl->data()->Terminate();
    }

    IhullTypeIGC*   pht = pship->GetBaseHullType();
    for (PartTypeLinkIGC* l = pht->GetPreferredPartTypes()->first(); l; l = l->next())
    {
        IpartTypeIGC*   ppt = l->data();
        if (!pstation->CanBuy(ppt))
        {
            if (pszLeftOff[0])
                strncat_s(pszLeftOff, cbLeftOff, ", ", _TRUNCATE);
            strncat_s(pszLeftOff, cbLeftOff, ppt->GetName(), _TRUNCATE);
            continue;
        }
        ppt = (IpartTypeIGC*)(pstation->GetSuccessor(ppt));
        EquipmentType   et = ppt->GetEquipmentType();
        Mount           mountMax = (et == ET_Weapon) ? pht->GetMaxWeapons() : 1;
        for (Mount i = 0; i < mountMax; i++)
        {
            if ((pship->GetMountedPart(et, i) == NULL) && pht->CanMount(ppt, i))
                pship->CreateAndAddPart(ppt, i, 0x7fff);
        }
    }
}

// Clients count a ship as human when it has no drone type, so give combat
// ships the core's wingman drone type, or failing that any drone type.
static DroneTypeID AIWingmanDroneType(ImissionIGC* pmission)
{
    DroneTypeID dtidOther = NA;
    for (DroneTypeLinkIGC* l = pmission->GetDroneTypes()->first(); l; l = l->next())
    {
        IdroneTypeIGC*  pdt = l->data();
        if (pdt->GetPilotType() == c_ptWingman)
            return pdt->GetObjectID();
        if (dtidOther == NA)
            dtidOther = pdt->GetObjectID();
    }
    return dtidOther;
}

static IshipIGC* AINearest(IshipIGC* pship, std::vector<IshipIGC*>& targets)
{
    // A docked ship is where its station is.
    IstationIGC*    pstation = pship->GetStation();
    IclusterIGC*    pcluster = pstation ? pstation->GetCluster() : pship->GetCluster();
    Vector          position = pstation ? pstation->GetPosition() : pship->GetPosition();

    IshipIGC*   pbest = NULL;
    float       distanceBest = FLT_MAX;
    for (size_t i = 0; i < targets.size(); i++)
    {
        float   distance = (targets[i]->GetCluster() == pcluster)
                           ? (targets[i]->GetPosition() - position).Length()
                           : 1.0e6f;
        if (distance < distanceBest)
        {
            distanceBest = distance;
            pbest = targets[i];
        }
    }
    return pbest;
}

// Our first station that sells this hull and has nothing better instead.
static IstationIGC* AIStationFor(AISurvey& s, IhullTypeIGC* pht)
{
    for (size_t i = 0; i < s.ourStations.size(); i++)
    {
        if (s.ourStations[i]->CanBuy(pht) && !s.ourStations[i]->IsObsolete(pht))
            return s.ourStations[i];
    }
    return NULL;
}

static bool AIOverlaps(const TechTreeBitMask& a, const TechTreeBitMask& b)
{
    TechTreeBitMask t = a;
    t &= b;
    return !t.GetAllZero();
}

static bool AISubset(const TechTreeBitMask& a, const TechTreeBitMask& b)
{
    TechTreeBitMask t = a;
    t -= b;
    return t.GetAllZero();
}

static bool AIContains(std::vector<IshipIGC*>& ships, ImodelIGC* pmodel)
{
    for (size_t i = 0; i < ships.size(); i++)
    {
        if ((ImodelIGC*)ships[i] == pmodel)
            return true;
    }
    return false;
}

// Busy = on one of our errands: attacking, or flying to the waypoint outside the
// station we're attacking. Anything else, drones' own default orders included, can be replaced.
static bool AIBusy(IshipIGC* pship, IclusterIGC* pclusterTarget)
{
    ImodelIGC*  ptarget = pship->GetCommandTarget(c_cmdAccepted);
    if ((ptarget == NULL) || (ptarget->GetCluster() == NULL))
        return false;

    CommandID   cid = pship->GetCommandID(c_cmdAccepted);
    if (cid == c_cidAttack)
        return true;
    return (cid == c_cidGoto) && (ptarget->GetObjectType() == OT_buoy) &&
           (pclusterTarget != NULL) && (ptarget->GetCluster() == pclusterTarget);
}

// A waypoint just outside an enemy station, on the side of the aleph we'd come in by.
// Drones won't take an attack order on a station their side hasn't seen, but they
// will fly to a waypoint, and from there they see it.
static IbuoyIGC* AIApproachBuoy(ImissionIGC* pmission, AISurvey& s, IstationIGC* pstation)
{
    IclusterIGC*    pcluster = pstation->GetCluster();
    const Vector&   position = pstation->GetPosition();

    IwarpIGC*       pwarpBest = NULL;
    int             jumpsBest = INT_MAX;
    for (WarpLinkIGC* l = pcluster->GetWarps()->first(); l; l = l->next())
    {
        IwarpIGC*   pwarp = l->data();
        IwarpIGC*   pwarpOther = pwarp->GetDestination();
        if ((pwarpOther == NULL) || (pwarpOther->GetCluster() == NULL))
            continue;

        int jumps = AIJumps(s, pwarpOther->GetCluster());
        if (jumps < jumpsBest)
        {
            jumpsBest = jumps;
            pwarpBest = pwarp;
        }
    }

    Vector  direction = pwarpBest ? (pwarpBest->GetPosition() - position) : Vector::RandomDirection();
    if (direction.LengthSquared() < 1.0f)
        direction = Vector::RandomDirection();

    DataBuoyIGC db;
    db.position = position + direction.Normalize() * (pstation->GetRadius() + 600.0f);
    db.clusterID = pcluster->GetObjectID();
    db.type = c_buoyWaypoint;
    db.buoyID = pmission->GenerateNewBuoyID();
    db.visible = false;
    return (IbuoyIGC*)(pmission->CreateObject(pmission->GetLastUpdate(), OT_buoy, &db, sizeof(db)));
}

// One attack: the target station and how to get at it.
struct AIAttack
{
    AIAttack() : pstation(NULL), bSeen(false), iGuard(0), pbuoy(NULL), cBuoy(0) {}

    IstationIGC*            pstation;
    bool                    bSeen;      // our side has seen the station, so drones can attack it
    std::vector<IshipIGC*>  guards;     // enemy ships we can see in its sector
    size_t                  iGuard;     // spreads fighters over the guards
    IbuoyIGC*               pbuoy;      // approach waypoint, made when first needed
    int                     cBuoy;      // ships sent to it
};

/*-------------------------------------------------------------------------
 * AICommander
 */
AICommander::AICommander(CFSMission* pfsMission, SideID sideID)
  : m_pfsMission(pfsMission),
    m_sideID(sideID),
    m_bStarted(false),
    m_timeNow(Time::Now()),
    m_timeStart(m_timeNow),
    m_timeNextThink(m_timeNow),
    m_timeNextReport(m_timeNow),
    m_timeLastChat(m_timeNow),
    m_timeLastWave(m_timeNow),
    m_moneySpent(0),
    m_moneyResearch(0),
    m_moneyEarned(-1),
    m_moneyEarnedReport(-1),
    m_potShips(0),
    m_potResearch(0),
    m_cMinersBought(0),
    m_cStationsBought(0),
    m_cResearchBought(0),
    m_cShipsBought(0),
    m_cWaves(0),
    m_cOurStations(-1),
    m_cEnemyStations(-1),
    m_idResearchNext(NA)
{
    m_szSideName[0] = '\0';
    m_szHulls[0] = '\0';
    m_szResearch[0] = '\0';
    m_szStations[0] = '\0';
    AIGetOptions(true);
}

AICommander::~AICommander()
{
    if (m_bStarted)
        Log("stands down: spent %d (%d on research); bought %d combat ships, %d miners, %d stations, %d research; %d attack waves",
            m_moneySpent, m_moneyResearch, m_cShipsBought, m_cMinersBought, m_cStationsBought, m_cResearchBought, m_cWaves);
}

IsideIGC* AICommander::GetSide() const
{
    return m_pfsMission->GetIGCMission()->GetSide(m_sideID);
}

void AICommander::Log(const char* format, ...)
{
    char    bfr[1400];
    va_list va;
    va_start(va, format);
    _vsnprintf_s(bfr, sizeof(bfr), _TRUNCATE, format, va);
    va_end(va);

    int seconds = m_bStarted ? (int)(m_timeNow - m_timeStart) : 0;
    AILog("[t=%02d:%02d] %-14s %s", seconds / 60, seconds % 60, m_szSideName, bfr);
}

// Chat to the game as "[AI side] text". Routine messages are held back to one
// every few seconds so the AI doesn't flood the chat; all of them are logged.
void AICommander::Chat(bool bForce, const char* format, ...)
{
    char    bfr[200];
    va_list va;
    va_start(va, format);
    _vsnprintf_s(bfr, sizeof(bfr), _TRUNCATE, format, va);
    va_end(va);

    Log("says: %s", bfr);

    const AIOptions&    o = AIGetOptions();
    if ((o.chat == 0) || (!bForce && ((m_timeNow - m_timeLastChat) < 5.0f)))
        return;
    m_timeLastChat = m_timeNow;

    char    bfrChat[260];
    sprintf_s(bfrChat, sizeof(bfrChat), "[AI %s] %s", m_szSideName, bfr);
    if (o.chat >= 2)
        m_pfsMission->GetSite()->SendChat(NULL, CHAT_EVERYONE, NA, NA, bfrChat);
    else
        m_pfsMission->GetSite()->SendChat(NULL, CHAT_TEAM, m_sideID, NA, bfrChat);
}

void AICommander::Tick(Time now)
{
    m_timeNow = now;
    if (m_bStarted && (now < m_timeNextThink))
        return;

    IsideIGC*   pside = GetSide();
    if ((pside == NULL) || !pside->GetActiveF())
        return;

    const AIOptions&    o = AIGetOptions();
    bool                bFirst = !m_bStarted;
    if (bFirst)
    {
        m_bStarted = true;
        m_timeStart = now;
        m_timeNextReport = now;
        m_timeLastChat = now - 60.0f;
        strncpy_s(m_szSideName, sizeof(m_szSideName), pside->GetName(), _TRUNCATE);

        Log("takes command with %d money", m_pfsMission->GetSideMoney(m_sideID));
        Chat(true, "AI commander online. Building an economy, then coming for you.");
    }
    m_timeNextThink = now + (float)o.thinkSeconds;

    AISurvey    survey;
    TakeSurvey(pside, survey);
    FindWantedTechs(pside, survey);
    LogHulls(survey, bFirst);
    OrderMiners(survey);
    OrderBuilders(survey);
    Spend(pside, survey);
    OrderCombatShips(pside, survey);

    int     cOurStations = (int)survey.ourStations.size();
    int     cEnemyStations = (int)survey.enemyStations.size();
    if (m_cOurStations >= 0)
    {
        if (cEnemyStations < m_cEnemyStations)
        {
            Log("enemy stations down to %d", cEnemyStations);
            Chat(true, "Enemy station destroyed, %d to go.", cEnemyStations);
        }
        if (cOurStations < m_cOurStations)
            Log("lost a station, %d left", cOurStations);
    }
    m_cOurStations = cOurStations;
    m_cEnemyStations = cEnemyStations;

    if (now >= m_timeNextReport)
    {
        m_timeNextReport = now + 60.0f;
        Report(survey);
    }
}

static const char* AICommandName(CommandID cid)
{
    return ((cid >= 0) && (cid < c_cidMax)) ? c_cdAllCommands[cid].szVerb : "none";
}

static const char* AIObjectName(ImodelIGC* pmodel)
{
    if (pmodel == NULL)
        return "-";

    switch (pmodel->GetObjectType())
    {
        case OT_asteroid:   return "rock";
        case OT_station:    return "station";
        case OT_warp:       return "aleph";
        case OT_buoy:       return "buoy";
        case OT_ship:       return "ship";
        default:            return "other";
    }
}

// " (unseen)" for a target the ship can't see, which only the AI's own lookup
// (FedSrvSiteBase::GetCluster) lets it fly to; for the reports.
static const char* AIUnseen(IshipIGC* pship, ImodelIGC* ptarget)
{
    return (ptarget && !pship->CanSee(ptarget)) ? " (unseen)" : "";
}

void AICommander::Report(AISurvey& s)
{
    Money   money = m_pfsMission->GetSideMoney(m_sideID);
    Money   earned = money + m_moneySpent;
    Money   income = (m_moneyEarnedReport >= 0) ? (earned - m_moneyEarnedReport) : 0;
    m_moneyEarnedReport = earned;

    // What the miners in space are doing: going for a rock, going home to unload, or neither.
    int     cMining = 0;
    int     cUnloading = 0;
    float   ore = 0.0f;
    char    bfr[1200];
    int     n = 0;
    bfr[0] = '\0';
    for (size_t i = 0; i < s.miners.size(); i++)
    {
        IshipIGC*   pship = s.miners[i];
        ImodelIGC*  ptarget = pship->GetCommandTarget(c_cmdCurrent);
        if (ptarget == NULL)
            ptarget = pship->GetCommandTarget(c_cmdAccepted);
        ObjectType  type = ptarget ? ptarget->GetObjectType() : OT_invalid;
        if (type == OT_asteroid)
            cMining++;
        else if (type == OT_station)
            cUnloading++;
        ore += pship->GetOre();

        if (n < (int)sizeof(bfr) - 100)
        {
            int c = _snprintf_s(bfr + n, sizeof(bfr) - n, _TRUNCATE, "%s%s %s>%s%s %.0f ore in %s at %.0f m/s", (n == 0) ? "" : "; ",
                                pship->GetName(),
                                AICommandName(pship->GetCommandID(c_cmdCurrent)),
                                AIObjectName(ptarget), AIUnseen(pship, ptarget), pship->GetOre(),
                                pship->GetCluster() ? pship->GetCluster()->GetName() : "-",
                                pship->GetVelocity().Length());
            if (c > 0)
                n += c;
        }
    }
    for (size_t i = 0; i < s.minersDocked.size(); i++)
    {
        IshipIGC*   pship = s.minersDocked[i];
        if (n < (int)sizeof(bfr) - 100)
        {
            int c = _snprintf_s(bfr + n, sizeof(bfr) - n, _TRUNCATE, "%s%s docked in %s, %s>%s", (n == 0) ? "" : "; ",
                                pship->GetName(), pship->GetStation()->GetName(),
                                AICommandName(pship->GetCommandID(c_cmdAccepted)),
                                AIObjectName(pship->GetCommandTarget(c_cmdAccepted)));
            if (c > 0)
                n += c;
        }
    }

    Log("money %d (%+d a minute) | stations %d (+%d coming) | miners %d (+%d): %d mining, %d unloading, %d docked, %.0f ore | combat %d (%d bombers, %d docked) | research %d under way | set aside %d ships, %d research | spent %d (%d research) | waves %d",
        money, income,
        s.cStations, s.cBuilders + s.cStationsBuilding,
        s.cMiners, s.cMinersBuilding, cMining, cUnloading, s.cMiners - (int)s.miners.size(), ore,
        s.cCombat, s.cBombers, (int)s.combatDocked.size(),
        s.cResearching,
        m_potShips, m_potResearch,
        m_moneySpent, m_moneyResearch,
        m_cWaves);
    if (n > 0)
        Log("miners: %s", bfr);

    // What the constructors in space are doing, and how far they still have to go.
    n = 0;
    bfr[0] = '\0';
    for (size_t i = 0; i < s.builders.size(); i++)
    {
        IshipIGC*       pship = s.builders[i];
        ImodelIGC*      ptarget = pship->GetCommandTarget(c_cmdAccepted);
        IclusterIGC*    pcluster = pship->GetCluster();
        IclusterIGC*    pclusterTarget = ptarget ? ptarget->GetCluster() : NULL;
        char            szAway[40];
        if (ptarget && (pclusterTarget == pcluster))
            sprintf_s(szAway, sizeof(szAway), "%.0f m away", (ptarget->GetPosition() - pship->GetPosition()).Length());
        else
            sprintf_s(szAway, sizeof(szAway), "%d jumps out", pclusterTarget ? AIJumps(s, pclusterTarget) : -1);

        if (n < (int)sizeof(bfr) - 100)
        {
            int c = _snprintf_s(bfr + n, sizeof(bfr) - n, _TRUNCATE, "%s%s %s>%s%s in %s, now %s>%s, in %s at %.0f m/s, %s", (n == 0) ? "" : "; ",
                                pship->GetName(),
                                AICommandName(pship->GetCommandID(c_cmdAccepted)), AIObjectName(ptarget), AIUnseen(pship, ptarget),
                                pclusterTarget ? pclusterTarget->GetName() : "-",
                                AICommandName(pship->GetCommandID(c_cmdCurrent)), AIObjectName(pship->GetCommandTarget(c_cmdCurrent)),
                                pcluster->GetName(), pship->GetVelocity().Length(), szAway);
            if (c > 0)
                n += c;
        }
    }
    if ((int)s.builders.size() < s.cBuilders)
    {
        int c = _snprintf_s(bfr + n, sizeof(bfr) - n, _TRUNCATE, "%s%d docked", (n == 0) ? "" : "; ", s.cBuilders - (int)s.builders.size());
        if (c > 0)
            n += c;
    }
    if (n > 0)
        Log("builders: %s", bfr);
}

void AICommander::TakeSurvey(IsideIGC* pside, AISurvey& s)
{
    ImissionIGC*    pmission = m_pfsMission->GetIGCMission();
    s.cShipsInMission = pmission->GetShips()->n();

    for (StationLinkIGC* l = pmission->GetStations()->first(); l; l = l->next())
    {
        IstationIGC*    pstation = l->data();
        IclusterIGC*    pcluster = pstation->GetCluster();
        if (pcluster == NULL)
            continue;

        if (pstation->GetSide() == pside)
        {
            s.ourStations.push_back(pstation);
            s.ourClusters.insert(pcluster);
            s.typeCount[pstation->GetStationType()]++;
        }
        else
        {
            s.enemyStations.push_back(pstation);
            s.enemyClusters.insert(pcluster);
        }
    }
    s.cStations = (int)s.ourStations.size();

    for (ShipLinkIGC* l = pside->GetShips()->first(); l; l = l->next())
    {
        IshipIGC*   pship = l->data();
        bool        bInSpace = (pship->GetCluster() != NULL);
        switch (pship->GetPilotType())
        {
            case c_ptMiner:
            {
                s.cMiners++;
                if (bInSpace)
                    s.miners.push_back(pship);
                else if (pship->GetStation() != NULL)
                    s.minersDocked.push_back(pship);

                ImodelIGC*  prock = pship->GetCommandTarget(c_cmdAccepted);
                if (prock && (prock->GetObjectType() == OT_asteroid))
                    s.minersOnRock[prock]++;
            }
            break;

            case c_ptBuilder:
            {
                s.cBuilders++;
                if (bInSpace)
                    s.builders.push_back(pship);

                ImodelIGC*  prock = pship->GetCommandTarget(c_cmdAccepted);
                if (prock && (pship->GetCommandID(c_cmdAccepted) == c_cidBuild))
                    s.rocksTaken.insert(prock);

                IbaseIGC*   pbase = pship->GetBaseData();
                if (pbase)
                    s.typeCount[(const IstationTypeIGC*)pbase]++;
            }
            break;

            case c_ptWingman:
            {
                s.cCombat++;
                if (AIIsBomber(pship->GetBaseHullType()))
                    s.cBombers++;
                if (bInSpace)
                    s.combat.push_back(pship);
                else if (pship->GetStation() != NULL)
                    s.combatDocked.push_back(pship);
            }
            break;

            default:
            break;
        }
    }

    for (BucketLinkIGC* l = pside->GetBuckets()->first(); l; l = l->next())
    {
        IbucketIGC* pbucket = l->data();
        if ((pbucket->GetMoney() == 0) || pbucket->GetCompleteF())
            continue;

        switch (pbucket->GetBucketType())
        {
            case OT_development:
                s.cResearching++;
            break;

            case OT_droneType:
                if (((IdroneTypeIGC*)(pbucket->GetBuyable()))->GetPilotType() == c_ptMiner)
                    s.cMinersBuilding++;
            break;

            case OT_stationType:
                s.cStationsBuilding++;
                s.typeCount[(const IstationTypeIGC*)(pbucket->GetBuyable())]++;
            break;

            default:
            break;
        }
    }

    // Jumps out from our stations, through every aleph we know of (the drones
    // cannot find a way through the rest).
    std::vector<IclusterIGC*>   queue(s.ourClusters.begin(), s.ourClusters.end());
    for (size_t i = 0; i < queue.size(); i++)
        s.jumps[queue[i]] = 0;
    for (size_t i = 0; i < queue.size(); i++)
    {
        int jumps = s.jumps[queue[i]] + 1;
        for (WarpLinkIGC* l = queue[i]->GetWarps()->first(); l; l = l->next())
        {
            IwarpIGC*   pwarpDestination = l->data()->SeenBySide(pside) ? l->data()->GetDestination() : NULL;
            IclusterIGC* pcluster = pwarpDestination ? pwarpDestination->GetCluster() : NULL;
            if (pcluster && (s.jumps.find(pcluster) == s.jumps.end()))
            {
                s.jumps[pcluster] = jumps;
                queue.push_back(pcluster);
            }
        }
    }

    // Enemy ships we can see, and the sectors where armed ones are or lately were.
    for (ShipLinkIGC* l = pmission->GetShips()->first(); l; l = l->next())
    {
        IshipIGC*   pship = l->data();
        IsideIGC*   psideShip = pship->GetSide();
        if ((pship->GetCluster() == NULL) || (psideShip == NULL) || (psideShip == pside) ||
            (psideShip->GetObjectID() < 0) || pship->IsGhost() || AIIsLifepod(pship) ||
            !pship->SeenBySide(pside))
            continue;

        s.enemies.push_back(pship);
        if (AIIsArmed(pship))
        {
            IclusterIGC*    pcluster = pship->GetCluster();
            if ((m_timeThreat.find(pcluster->GetObjectID()) == m_timeThreat.end()) &&
                (s.enemyClusters.find(pcluster) == s.enemyClusters.end()))
                Log("raiders seen in %s", pcluster->GetName());
            m_timeThreat[pcluster->GetObjectID()] = m_timeNow;
            s.armedClusters.insert(pcluster);
        }
    }
    for (std::map<SectorID, Time>::iterator it = m_timeThreat.begin(); it != m_timeThreat.end(); )
    {
        IclusterIGC*    pcluster = pmission->GetCluster(it->first);
        if ((pcluster == NULL) || ((m_timeNow - it->second) > 45.0f))
        {
            if (pcluster && (s.enemyClusters.find(pcluster) == s.enemyClusters.end()))
                Log("%s clear of raiders", pcluster->GetName());
            m_timeThreat.erase(it++);
        }
        else
        {
            s.threatClusters.insert(pcluster);
            ++it;
        }
    }
}

void AICommander::OrderMiners(AISurvey& s)
{
    // Armed enemies outside the sectors they hold are raiders.
    bool    bRaiders = false;
    for (std::set<IclusterIGC*>::iterator it = s.threatClusters.begin(); it != s.threatClusters.end(); ++it)
    {
        if (s.enemyClusters.find(*it) == s.enemyClusters.end())
            bRaiders = true;
    }

    // Forget the miners we pulled back that have since docked, died or had their orders changed.
    std::set<ObjectID>  pulledBack;
    for (size_t i = 0; i < s.miners.size(); i++)
    {
        IshipIGC*   pship = s.miners[i];
        ImodelIGC*  ptarget = pship->GetCommandTarget(c_cmdAccepted);
        if ((m_minersPulledBack.find(pship->GetObjectID()) != m_minersPulledBack.end()) &&
            (pship->GetCommandID(c_cmdAccepted) == c_cidGoto) && ptarget && (ptarget->GetObjectType() == OT_station))
            pulledBack.insert(pship->GetObjectID());
    }
    m_minersPulledBack.swap(pulledBack);

    for (size_t i = 0; i < s.miners.size(); i++)
    {
        IshipIGC*   pship = s.miners[i];
        CommandID   cidAccepted = pship->GetCommandID(c_cmdAccepted);
        CommandID   cidCurrent = pship->GetCommandID(c_cmdCurrent);
        ImodelIGC*  ptarget = pship->GetCommandTarget(c_cmdAccepted);

        // Raiders here, or where it is going to mine: off to a safe rock, or home
        // to sit tight until they are gone.
        bool    bThreatHere = (s.threatClusters.find(pship->GetCluster()) != s.threatClusters.end());
        bool    bThreatThere = (cidAccepted == c_cidMine) && ptarget && (ptarget->GetCluster() != NULL) &&
                               (s.threatClusters.find(ptarget->GetCluster()) != s.threatClusters.end());
        if (bThreatHere || bThreatThere)
        {
            if (!bThreatHere)
            {
                IasteroidIGC*   prock = FindMineRock(s, pship);
                if (prock && Order(pship, c_cidMine, prock))
                {
                    s.minersOnRock[prock]++;
                    Log("sends %s away from raiders in %s to mine in %s", pship->GetName(),
                        ptarget->GetCluster()->GetName(), prock->GetCluster()->GetName());
                    continue;
                }
            }

            IstationIGC*    pstation = AIRefuge(s, pship);
            if (pstation && OrderNew(pship, c_cidGoto, pstation))
            {
                m_minersPulledBack.insert(pship->GetObjectID());
                Log("pulls %s back to %s, raiders in %s", pship->GetName(), pstation->GetName(),
                    (bThreatHere ? pship->GetCluster() : ptarget->GetCluster())->GetName());
            }
            pship->SetStayDocked(true);     // after the order, which clears it
            continue;
        }

        // On its way home from raiders that are gone now: back to work, if there is a safe rock.
        // A miner going home to unload has the same orders, from the stock miner code; leave it be.
        if ((cidAccepted == c_cidGoto) && ptarget && (ptarget->GetObjectType() == OT_station) &&
            (m_minersPulledBack.find(pship->GetObjectID()) != m_minersPulledBack.end()))
        {
            IasteroidIGC*   prock = FindMineRock(s, pship);
            if (prock && Order(pship, c_cidMine, prock))
            {
                m_minersPulledBack.erase(pship->GetObjectID());
                s.minersOnRock[prock]++;
                Log("sends %s back out to mine in %s", pship->GetName(), prock->GetCluster()->GetName());
            }
            continue;
        }

        bool    bIdle = ((cidAccepted == c_cidNone) || (cidAccepted == c_cidDoNothing) ||
                         ((cidAccepted == c_cidMine) && ((ptarget == NULL) || (ptarget->GetCluster() == NULL)))) &&
                        ((cidCurrent == c_cidNone) || (cidCurrent == c_cidDoNothing));
        if (!bIdle)
            continue;

        IasteroidIGC*   prock = FindMineRock(s, pship);
        if (prock && Order(pship, c_cidMine, prock))
        {
            s.minersOnRock[prock]++;
            Log("sends %s to mine in %s", pship->GetName(), prock->GetCluster()->GetName());
        }
    }

    // A miner back from unloading sits in the station until its orders make sense
    // (see IshipIGC::OkToLaunch), and the stock default order finds no rock for it.
    // It stays in while raiders are outside, or while they are about and it has no safe rock.
    for (size_t i = 0; i < s.minersDocked.size(); i++)
    {
        IshipIGC*   pship = s.minersDocked[i];
        if (s.threatClusters.find(pship->GetStation()->GetCluster()) != s.threatClusters.end())
        {
            pship->SetStayDocked(true);
            continue;
        }

        ImodelIGC*  ptarget = pship->GetCommandTarget(c_cmdAccepted);
        if ((pship->GetCommandID(c_cmdAccepted) == c_cidMine) && ptarget &&
            (ptarget->GetObjectType() == OT_asteroid) && (ptarget->GetCluster() != NULL) &&
            (s.threatClusters.find(ptarget->GetCluster()) == s.threatClusters.end()) &&
            (((IasteroidIGC*)ptarget)->GetOre() >= 1.0f))
        {
            pship->SetStayDocked(false);    // it has a rock, and launches for it on its own
            continue;
        }

        IasteroidIGC*   prock = FindMineRock(s, pship);
        if (prock && Order(pship, c_cidMine, prock))
        {
            s.minersOnRock[prock]++;
            Log("sends %s out of %s to mine in %s", pship->GetName(),
                pship->GetStation()->GetName(), prock->GetCluster()->GetName());
        }
        else
            pship->SetStayDocked(bRaiders);     // nothing safe to mine: sit tight while raiders are about
    }
}

IasteroidIGC* AICommander::FindMineRock(AISurvey& s, IshipIGC* pship)
{
    // Measure from the miner, or from its station when docked.
    IclusterIGC*    pclusterFrom = pship->GetCluster();
    Vector          posFrom = pship->GetPosition();
    if ((pclusterFrom == NULL) && (pship->GetStation() != NULL))
    {
        pclusterFrom = pship->GetStation()->GetCluster();
        posFrom = pship->GetStation()->GetPosition();
    }

    IasteroidIGC*   pbest = NULL;
    float           scoreBest = FLT_MAX;
    for (AsteroidLinkIGC* l = m_pfsMission->GetIGCMission()->GetAsteroids()->first(); l; l = l->next())
    {
        IasteroidIGC*   prock = l->data();
        IclusterIGC*    pcluster = prock->GetCluster();
        if ((pcluster == NULL) || !prock->HasCapability(c_aabmMineHe3) || (prock->GetOre() < 1.0f) ||
            (s.enemyClusters.find(pcluster) != s.enemyClusters.end()) ||
            (s.threatClusters.find(pcluster) != s.threatClusters.end()))
            continue;

        int jumps = AIJumps(s, pcluster);
        if (jumps > 2)
            continue;

        // Close to our stations, not crowded, then close to the miner.
        float   score = 20000.0f * jumps + 4000.0f * s.minersOnRock[prock];
        score += (pcluster == pclusterFrom)
                 ? (prock->GetPosition() - posFrom).Length()
                 : 10000.0f;
        if (score < scoreBest)
        {
            scoreBest = score;
            pbest = prock;
        }
    }
    return pbest;
}

void AICommander::OrderBuilders(AISurvey& s)
{
    for (size_t i = 0; i < s.builders.size(); i++)
    {
        IshipIGC*   pship = s.builders[i];
        ImodelIGC*  ptarget = pship->GetCommandTarget(c_cmdAccepted);
        if ((pship->GetCommandID(c_cmdAccepted) == c_cidBuild) && ptarget && ptarget->GetCluster())
            continue;

        IstationTypeIGC*    pst = (IstationTypeIGC*)(pship->GetBaseData());
        if (pst == NULL)
            continue;

        IasteroidIGC*   prock = FindBuildRock(s, pst->GetBuildAABM());
        if (prock == NULL)
        {
            // Fall back on the rock the constructor picked for itself.
            ImodelIGC*  pcurrent = pship->GetCommandTarget(c_cmdCurrent);
            if (pcurrent && (pcurrent->GetObjectType() == OT_asteroid) && pcurrent->GetCluster())
                prock = (IasteroidIGC*)pcurrent;
        }

        if (prock && Order(pship, c_cidBuild, prock))
        {
            s.rocksTaken.insert(prock);
            Chat(false, "Building a %s in %s.", pst->GetName(), prock->GetCluster()->GetName());
        }
    }
}

IasteroidIGC* AICommander::FindBuildRock(AISurvey& s, AsteroidAbilityBitMask aabm)
{
    IasteroidIGC*   pbest = NULL;
    int             scoreBest = INT_MAX;
    for (AsteroidLinkIGC* l = m_pfsMission->GetIGCMission()->GetAsteroids()->first(); l; l = l->next())
    {
        IasteroidIGC*   prock = l->data();
        IclusterIGC*    pcluster = prock->GetCluster();
        if ((pcluster == NULL) || !prock->HasCapability(aabm) ||
            (s.rocksTaken.find(prock) != s.rocksTaken.end()))
            continue;

        int jumps = AIJumps(s, pcluster);
        if (jumps >= 1000)
            continue;

        // Next door is best, since it claims new ground; then home, then
        // further out. Stay out of sectors the enemy holds.
        int score = (jumps == 0) ? 15 : (10 * jumps);
        if (s.enemyClusters.find(pcluster) != s.enemyClusters.end())
            score += 100;

        if (score < scoreBest)
        {
            scoreBest = score;
            pbest = prock;
        }
    }
    return pbest;
}

void AICommander::Spend(IsideIGC* pside, AISurvey& s)
{
    const AIOptions&    o = AIGetOptions();

    // What can be bought right now.
    IbucketIGC*                 pbucketMiner = NULL;
    IbucketIGC*                 pbucketResearch = NULL;     // the cheapest research
    IbucketIGC*                 pbucketWanted = NULL;       // the cheapest research toward better ships
    std::vector<IbucketIGC*>    researchBuckets;
    std::vector<IbucketIGC*>    stationBuckets;
    for (BucketLinkIGC* l = pside->GetBuckets()->first(); l; l = l->next())
    {
        IbucketIGC*     pbucket = l->data();
        IbuyableIGC*    pbuyable = pbucket->GetBuyable();
        if ((pbucket->GetMoney() != 0) || (pbucket->GetPrice() <= 0) ||
            (pbuyable->GetGroupID() < 0) || !pside->CanBuy(pbuyable))
            continue;

        switch (pbucket->GetBucketType())
        {
            case OT_droneType:
                if ((((IdroneTypeIGC*)pbuyable)->GetPilotType() == c_ptMiner) &&
                    ((pbucketMiner == NULL) || (pbucket->GetPrice() < pbucketMiner->GetPrice())))
                    pbucketMiner = pbucket;
            break;

            case OT_stationType:
                stationBuckets.push_back(pbucket);
            break;

            case OT_development:
                if (!pbucket->GetCompleteF() &&
                    !((IdevelopmentIGC*)pbuyable)->IsObsolete(pside->GetDevelopmentTechs()))
                {
                    researchBuckets.push_back(pbucket);
                    if ((pbucketResearch == NULL) || (pbucket->GetPrice() < pbucketResearch->GetPrice()))
                        pbucketResearch = pbucket;
                    if (AIOverlaps(pbuyable->GetEffectTechs(), s.ttbmWanted) &&
                        ((pbucketWanted == NULL) || (pbucket->GetPrice() < pbucketWanted->GetPrice())))
                        pbucketWanted = pbucket;
                }
            break;

            default:
            break;
        }
    }

    // Shares of income are set aside for combat ships and for research, so the army and
    // the tech keep growing while the rest is saved up for miners and stations. The money
    // the side started with is not shared out.
    Money   earned = m_pfsMission->GetSideMoney(m_sideID) + m_moneySpent;
    if (m_moneyEarned >= 0)
    {
        Money   income = earned - m_moneyEarned;
        if (income > 0)
        {
            m_potShips += (Money)(((__int64)income * o.shipPercent) / 100);
            m_potResearch += (Money)(((__int64)income * o.researchPercent) / 100);
        }
    }
    m_moneyEarned = earned;
    ClampPots();

    // Research toward better ships, unless that costs more than twice the cheapest and
    // its share can't pay for it yet; then the cheapest, so the tech never stalls.
    bool    bResearchWanted = false;
    if (pbucketWanted &&
        ((pbucketWanted->GetPrice() <= 2 * pbucketResearch->GetPrice()) || (pbucketWanted->GetPrice() <= m_potResearch)))
    {
        pbucketResearch = pbucketWanted;
        bResearchWanted = true;
    }
    LogResearch(s, researchBuckets, pbucketResearch, bResearchWanted);

    // 1. Without a couple of miners there is no economy: save everything for one,
    //    even before guards, or a side that lost its miners never gets them back.
    int cMinersFirst = (o.maxMiners < 2) ? (int)o.maxMiners : 2;
    if (pbucketMiner && ((s.cMiners + s.cMinersBuilding) < cMinersFirst) &&
        !BuyMiner(pside, s, pbucketMiner, 0))
        return;

    // 2. Research from its share, before guards bought from any money can eat into it.
    bool    bResearched = false;
    if (pbucketResearch == NULL)
        m_potResearch = 0;
    else if ((s.cResearching < (int)o.maxResearch) && (pbucketResearch->GetPrice() <= m_potResearch))
        bResearched = BuyResearch(pside, s, pbucketResearch, bResearchWanted, 0);

    // 3. A few combat ships at home, from any money.
    int cDefenders = (int)(o.attackGroupSize / 2);
    if (s.cCombat < cDefenders)
        BuyShipsFromPot(pside, s, 0, cDefenders - s.cCombat);

    // 4. Combat ships from their share.
    BuyShipsFromPot(pside, s, m_pfsMission->GetSideMoney(m_sideID) - m_potShips, 10);
    if (s.cCombat >= (int)o.maxShips)
        m_potShips = 0;

    // Money saved up for something worth waiting for; later wants only get what is left over.
    Money   savings = 0;

    // The station to buy next: more ground and more tech. A kind we lack that leads
    // to better ships comes first, then the kinds we have the fewest of, cheapest
    // first, and only where there is a rock for them.
    IbucketIGC*     pbucketStation = NULL;
    bool            bStationWanted = false;
    int cStationsComing = s.cBuilders + s.cStationsBuilding;
    if (((s.cStations + cStationsComing) < (int)o.maxStations) && (cStationsComing < (int)o.maxConstructors))
    {
        int             countBest = INT_MAX;
        char            szStations[sizeof(m_szStations)];
        szStations[0] = '\0';
        for (size_t i = 0; i < stationBuckets.size(); i++)
        {
            IbucketIGC*             pbucket = stationBuckets[i];
            const IstationTypeIGC*  pst = (const IstationTypeIGC*)(pbucket->GetBuyable());
            IasteroidIGC*           prock = FindBuildRock(s, pst->GetBuildAABM());

            char    bfr[2 * c_cbName + 30];
            sprintf_s(bfr, sizeof(bfr), "%s%s %d (%s%s)", i ? ", " : "", pst->GetName(), pbucket->GetPrice(),
                      prock ? "rock in " : "no rock", prock ? prock->GetCluster()->GetName() : "");
            strncat_s(szStations, sizeof(szStations), bfr, _TRUNCATE);
            if (prock == NULL)
                continue;

            int     count = s.typeCount[pst];
            bool    bWanted = (count == 0) &&
                              (AIOverlaps(pst->GetEffectTechs(), s.ttbmWanted) || AIOverlaps(pst->GetLocalTechs(), s.ttbmWanted));
            if ((pbucketStation == NULL) || (bWanted && !bStationWanted) ||
                ((bWanted == bStationWanted) &&
                 ((count < countBest) || ((count == countBest) && (pbucket->GetPrice() < pbucketStation->GetPrice())))))
            {
                countBest = count;
                pbucketStation = pbucket;
                bStationWanted = bWanted;
            }
        }

        if (strcmp(szStations, m_szStations) != 0)
        {
            strcpy_s(m_szStations, sizeof(m_szStations), szStations);
            Log("stations we can buy: %s", szStations[0] ? szStations : "none");
        }
    }

    // 5. More miners, from what is not set aside. Once there are half as many as we
    //    may have, the money for the next station is kept from them too, so buying
    //    miners doesn't keep putting off claiming more ground.
    bool    bMinersFirst = ((s.cMiners + s.cMinersBuilding) * 2) < (int)o.maxMiners;
    bool    bMinerSaved = false;
    if (pbucketMiner && ((s.cMiners + s.cMinersBuilding) < (int)o.maxMiners))
    {
        Money   reserve = m_potShips + m_potResearch;
        if (pbucketStation && !bMinersFirst)
            reserve += pbucketStation->GetPrice();
        bMinerSaved = !BuyMiner(pside, s, pbucketMiner, reserve);
    }

    // 6. The station, from what is not set aside, after the miner we are saving
    //    for while miners come first.
    if (pbucketStation)
    {
        Money   reserve = m_potShips + m_potResearch;
        if (bMinerSaved && bMinersFirst)
            reserve += pbucketMiner->GetPrice();
        if (BuyBucket(pside, pbucketStation, reserve))
        {
            m_cStationsBought++;
            s.cStationsBuilding++;
            s.typeCount[(const IstationTypeIGC*)(pbucketStation->GetBuyable())]++;
            Chat(true, "Ordered a %s constructor%s.", pbucketStation->GetBuyable()->GetName(),
                 bStationWanted ? " toward better ships" : "");
            ClampPots();
        }
        else
            savings += pbucketStation->GetPrice();
    }
    if (bMinerSaved)
        savings += pbucketMiner->GetPrice();

    // 7. Research from what is left over, along with its share.
    if (!bResearched && pbucketResearch && (s.cResearching < (int)o.maxResearch))
        BuyResearch(pside, s, pbucketResearch, bResearchWanted, m_potShips + savings);

    // 8. Everything else goes on combat ships, along with their share.
    BuyShipsFromPot(pside, s, m_potResearch + savings, 10);
}

// Keep what is set aside within the money the side has, ships first.
void AICommander::ClampPots(void)
{
    Money   money = m_pfsMission->GetSideMoney(m_sideID);
    if (m_potShips > money)
        m_potShips = money;
    if (m_potShips < 0)
        m_potShips = 0;
    if (m_potResearch > money - m_potShips)
        m_potResearch = money - m_potShips;
    if (m_potResearch < 0)
        m_potResearch = 0;
}

// Buy combat ships, leaving the reserve untouched, and take what they cost from
// the money set aside for them first.
int AICommander::BuyShipsFromPot(IsideIGC* pside, AISurvey& s, Money reserve, int cWanted)
{
    Money   spentBefore = m_moneySpent;
    int     cBought = BuyCombatShips(pside, s, reserve, cWanted);
    m_potShips -= m_moneySpent - spentBefore;
    ClampPots();
    return cBought;
}

bool AICommander::BuyMiner(IsideIGC* pside, AISurvey& s, IbucketIGC* pbucket, Money reserve)
{
    if (!BuyBucket(pside, pbucket, reserve))
        return false;

    m_cMinersBought++;
    s.cMinersBuilding++;
    Log("buys a miner for %d", pbucket->GetPrice());
    ClampPots();
    return true;
}

// Research, taking what it costs from the money set aside for research first.
bool AICommander::BuyResearch(IsideIGC* pside, AISurvey& s, IbucketIGC* pbucket, bool bWanted, Money reserve)
{
    Money   spent = BuyBucket(pside, pbucket, reserve);
    if (!spent)
        return false;

    m_moneyResearch += spent;
    m_cResearchBought++;
    s.cResearching++;
    m_potResearch -= spent;
    ClampPots();
    Chat(true, "Researching %s%s.", pbucket->GetBuyable()->GetName(), bWanted ? " toward better ships" : "");
    return true;
}

// Pay a bucket's whole price from the side's money, as a commander's donation
// would, if that leaves the reserve untouched. Returns what was paid.
Money AICommander::BuyBucket(IsideIGC* pside, IbucketIGC* pbucket, Money reserve)
{
    Money   price = pbucket->GetPrice() - pbucket->GetMoney();
    if ((price <= 0) || (price > m_pfsMission->GetSideMoney(m_sideID) - reserve))
        return 0;

    Money   spent = pbucket->AddMoney(price);
    m_pfsMission->SpendSideMoney(m_sideID, spent);
    m_moneySpent += spent;

    BEGIN_PFM_CREATE(g.fm, pfmBucketStatus, S, BUCKET_STATUS)
    END_PFM_CREATE
    pfmBucketStatus->timeTotal  = pbucket->GetTime();
    pfmBucketStatus->moneyTotal = pbucket->GetMoney();
    pfmBucketStatus->iBucket    = pbucket->GetObjectID();
    pfmBucketStatus->sideID     = m_sideID;
    g.fm.SendMessages(CFSSide::FromIGC(pside)->GetGroup(), FM_GUARANTEED, FM_FLUSH);

    return spent;
}

int AICommander::BuyCombatShips(IsideIGC* pside, AISurvey& s, Money reserve, int cWanted)
{
    const AIOptions&    o = AIGetOptions();

    int cBought = 0;
    while ((cBought < cWanted) && (cBought < 10) &&
           (s.cCombat < (int)o.maxShips) && (s.cShipsInMission < c_cDronesMax - 50))
    {
        Money   budget = m_pfsMission->GetSideMoney(m_sideID) - reserve;
        if (budget <= 0)
            break;

        bool    bBomber = ((s.cBombers + 1) * 100 <= (int)o.bomberPercent * (s.cCombat + 1));
        int     result = BuyCombatShip(pside, s, bBomber, budget);
        if (result < 0)
            result = BuyCombatShip(pside, s, !bBomber, budget);
        if (result <= 0)
            break;

        cBought++;
    }

    return cBought;
}

// Buy and launch the best combat ship of one class (bomber or not) that our
// stations can build: the dearest, and of hulls that cost the same, the toughest.
// Ships that cost under half the best of the class are not worth it, so the AI
// saves up instead. A ship bought while armed enemies are in its station's sector
// stays docked until OrderCombatShips sends it out with the others. Returns 1 when
// a ship was bought, 0 when the money isn't there yet, and -1 when no station can
// build the class.
int AICommander::BuyCombatShip(IsideIGC* pside, AISurvey& s, bool bBomber, Money budget)
{
    const AIOptions&    o = AIGetOptions();
    ImissionIGC*        pmission = m_pfsMission->GetIGCMission();

    IhullTypeIGC*   phtBest = NULL;
    IstationIGC*    pstationBest = NULL;
    Money           costBest = 0;
    Money           costTop = 0;
    float           strengthBest = 0.0f;
    for (HullTypeLinkIGC* l = pmission->GetHullTypes()->first(); l; l = l->next())
    {
        IhullTypeIGC*   pht = l->data();
        if (!AIIsCombatHull(pht) || (AIIsBomber(pht) != bBomber))
            continue;

        IstationIGC*    pstation = AIStationFor(s, pht);
        if (pstation == NULL)
            continue;

        Money   cost = AIShipCost(pht);
        if (cost > costTop)
            costTop = cost;
        float   strength = AIHullStrength(pht);
        if ((cost <= budget) && ((cost > costBest) || ((cost == costBest) && (strength > strengthBest))))
        {
            phtBest = pht;
            pstationBest = pstation;
            costBest = cost;
            strengthBest = strength;
        }
    }

    if (costTop == 0)
        return -1;
    if ((phtBest == NULL) || (costBest * 2 < costTop))
        return 0;

    IshipIGC*   pship = CreateDrone(pmission, -2, c_ptWingman, NULL, phtBest->GetObjectID(), pside, 0,
                                    (float)o.shootSkill / 100.0f, (float)o.moveSkill / 100.0f, (float)o.bravery / 100.0f);
    if (pship == NULL)
        return 0;
    char    bfrLeftOff[200];
    AIRefit(pship, pstationBest, bfrLeftOff, sizeof(bfrLeftOff));

    m_pfsMission->SpendSideMoney(m_sideID, costBest);
    m_moneySpent += costBest;
    m_cShipsBought++;

    // Set up like CreateStockDrone does for miners and constructors.
    pship->CreateDamageTrack();
    CFSDrone*   pfsDrone = new CFSDrone(pship);
    pfsDrone->SetDroneTypeID(AIWingmanDroneType(pmission));
    {
        char    bfr[c_cbName + 10];
        strncpy_s(bfr, c_cbName - 6, phtBest->GetName(), _TRUNCATE);
        size_t  cch = strlen(bfr);
        _itoa_s(pship->GetObjectID(), bfr + cch, sizeof(bfr) - cch, 10);
        pship->SetName(bfr);
    }
    SendPlayerInfo(NULL, pfsDrone, m_pfsMission);
    pfsDrone->ShipStatusStart(NULL);
    pfsDrone->SetLastDamageReport(pship->GetLastUpdate());

    // Fight anything met on the way, fire missiles, and only run when nearly dead.
    pship->SetWingmanBehaviour((WingmanBehaviourBitMask)((pship->GetWingmanBehaviour() & ~c_wbbmRunAt60Hull) |
                               c_wbbmRunAt30Hull | c_wbbmInRangeAggressive | c_wbbmUseMissiles));

    // Start at the station like everyone else, then launch, unless armed enemies are
    // in its sector: then it waits docked, to go out with the others (OrderCombatShips).
    bool    bHeld = (s.armedClusters.find(pstationBest->GetCluster()) != s.armedClusters.end());
    pship->SetStation(pstationBest);
    if (bHeld)
        pship->SetStayDocked(true);
    else
    {
        pship->PickDefaultOrder(pstationBest->GetCluster(), pstationBest->GetPosition(), true);
        pship->SetStation(NULL);
    }

    s.cCombat++;
    if (bBomber)
        s.cBombers++;
    s.cShipsInMission++;
    if (pship->GetCluster())
        s.combat.push_back(pship);
    else if (pship->GetStation())
        s.combatDocked.push_back(pship);

    Log("buys %s for %d at %s (%d combat ships)%s%s%s", pship->GetName(), costBest, pstationBest->GetName(), s.cCombat,
        bHeld ? ", kept docked, raiders outside" : "", bfrLeftOff[0] ? ", without " : "", bfrLeftOff);
    return 1;
}

void AICommander::OrderCombatShips(IsideIGC* pside, AISurvey& s)
{
    const AIOptions&    o = AIGetOptions();
    int                 cGuard = (int)(o.attackGroupSize / 2);

    // Ground to guard: sectors with our stations, those where our miners and
    // constructors are at work, and raided ones near home. Enemy ships there are
    // intruders: any ship in our stations' sectors, armed ones in the others.
    std::set<IclusterIGC*>  guarded(s.ourClusters);
    for (size_t i = 0; i < s.miners.size(); i++)
        guarded.insert(s.miners[i]->GetCluster());
    for (size_t i = 0; i < s.builders.size(); i++)
        guarded.insert(s.builders[i]->GetCluster());
    for (std::map<ImodelIGC*, int>::iterator it = s.minersOnRock.begin(); it != s.minersOnRock.end(); ++it)
    {
        if ((it->second > 0) && it->first->GetCluster())
            guarded.insert(it->first->GetCluster());
    }
    for (std::set<IclusterIGC*>::iterator it = s.threatClusters.begin(); it != s.threatClusters.end(); ++it)
    {
        if ((AIJumps(s, *it) <= 2) && (s.enemyClusters.find(*it) == s.enemyClusters.end()))
            guarded.insert(*it);
    }

    std::vector<IshipIGC*>& enemies = s.enemies;
    std::vector<IshipIGC*>  intruders;
    int                     cArmed = 0;
    for (size_t i = 0; i < enemies.size(); i++)
    {
        IclusterIGC*    pcluster = enemies[i]->GetCluster();
        bool            bArmed = AIIsArmed(enemies[i]);
        if ((s.ourClusters.find(pcluster) != s.ourClusters.end()) ||
            (bArmed && (guarded.find(pcluster) != guarded.end()) &&
             (s.enemyClusters.find(pcluster) == s.enemyClusters.end())))
        {
            intruders.push_back(enemies[i]);
            if (bArmed)
                cArmed++;
        }
    }

    // The enemy station closest to our ground, and what guards it.
    AIAttack        attack;
    IclusterIGC*    pclusterTarget = NULL;
    int             jumpsBest = INT_MAX;
    for (size_t i = 0; i < s.enemyStations.size(); i++)
    {
        int jumps = AIJumps(s, s.enemyStations[i]->GetCluster());
        if (jumps < jumpsBest)
        {
            jumpsBest = jumps;
            attack.pstation = s.enemyStations[i];
        }
    }
    if (attack.pstation)
    {
        pclusterTarget = attack.pstation->GetCluster();
        attack.bSeen = attack.pstation->SeenBySide(pside);
        for (size_t i = 0; i < enemies.size(); i++)
        {
            if (enemies[i]->GetCluster() == pclusterTarget)
                attack.guards.push_back(enemies[i]);
        }
    }

    // Ships on their way in: once the station has been seen, those flying to the
    // waypoint go for it (fighters for its guards), and fighters on the station
    // turn on guards that show up. Ships not busy with one of our errands are free.
    std::vector<IshipIGC*>  idleHome;       // in sectors with our stations
    std::vector<IshipIGC*>  idleField;      // anywhere else
    for (size_t i = 0; i < s.combat.size(); i++)
    {
        IshipIGC*   pship = s.combat[i];
        ImodelIGC*  ptarget = pship->GetCommandTarget(c_cmdAccepted);
        CommandID   cid = pship->GetCommandID(c_cmdAccepted);
        if (pclusterTarget && ptarget && (ptarget->GetCluster() == pclusterTarget))
        {
            bool    bBomber = AIIsBomber(pship->GetBaseHullType());
            if ((cid == c_cidGoto) && (ptarget->GetObjectType() == OT_buoy) && attack.bSeen)
                Order(pship, c_cidAttack, (bBomber || attack.guards.empty())
                                          ? (ImodelIGC*)attack.pstation
                                          : (ImodelIGC*)AINearest(pship, attack.guards));
            else if ((cid == c_cidAttack) && (ptarget == (ImodelIGC*)attack.pstation) &&
                     !bBomber && !attack.guards.empty())
                Order(pship, c_cidAttack, AINearest(pship, attack.guards));
        }

        if (AIBusy(pship, pclusterTarget))
            continue;

        if (s.ourClusters.find(pship->GetCluster()) != s.ourClusters.end())
            idleHome.push_back(pship);
        else
            idleField.push_back(pship);
    }

    // Ships that chased raiders out of our sectors stay near home, as home guards
    // that go out with the next wave, instead of heading for the enemy on their own.
    for (size_t i = 0; i < idleField.size(); )
    {
        IclusterIGC*    pcluster = idleField[i]->GetCluster();
        if ((AIJumps(s, pcluster) <= 1) && (pcluster != pclusterTarget) &&
            (s.enemyClusters.find(pcluster) == s.enemyClusters.end()))
        {
            idleHome.push_back(idleField[i]);
            idleField.erase(idleField.begin() + i);
        }
        else
            i++;
    }

    // Ships sitting in our stations: while armed enemies are in the sector outside, they
    // stay in until the defense below sends them out together; the others go back to work
    // (a docked drone with nothing to do would sit there for good).
    std::vector<IshipIGC*>  camped;         // docked, with armed enemies outside
    for (size_t i = 0; i < s.combatDocked.size(); i++)
    {
        IshipIGC*       pship = s.combatDocked[i];
        IstationIGC*    pstation = pship->GetStation();
        if (pstation && (s.armedClusters.find(pstation->GetCluster()) != s.armedClusters.end()))
            camped.push_back(pship);
        else
        {
            pship->SetStayDocked(false);
            if (!AIBusy(pship, pclusterTarget))
                idleHome.push_back(pship);
        }
    }

    // Defend first: two of ours for each armed intruder and one for each other,
    // from the ships at home, fighters before bombers, each to the intruder that
    // needs it most. When a raid is too big for them, attackers still on their way
    // out turn back, nearest home first. When even all of ours together are fewer
    // than the armed intruders, the ones not already fighting wait it out docked
    // (sending them one at a time just feeds the enemy), then go out all together.
    bool    bHold = false;
    if (!intruders.empty())
    {
        std::map<ImodelIGC*, int>   onIt;
        int                         cOnIt = 0;
        for (size_t i = 0; i < s.combat.size(); i++)
        {
            ImodelIGC*  ptarget = s.combat[i]->GetCommandTarget(c_cmdAccepted);
            if ((s.combat[i]->GetCommandID(c_cmdAccepted) == c_cidAttack) && AIContains(intruders, ptarget))
            {
                onIt[ptarget]++;
                cOnIt++;
            }
        }

        int cWanted = 2 * cArmed + ((int)intruders.size() - cArmed) + 1;

        // Everything we could put up against them: ships on them already, those at home
        // or docked, and the attackers we would call back.
        bool    bCanRecall = (cArmed >= cGuard) && (pclusterTarget != NULL);
        int     cRecallable = 0;
        for (size_t i = 0; bCanRecall && (i < s.combat.size()); i++)
        {
            IshipIGC*   pship = s.combat[i];
            ImodelIGC*  ptarget = pship->GetCommandTarget(c_cmdAccepted);
            bool        bOnTheWay = ptarget && (ptarget->GetCluster() == pclusterTarget) &&
                                    (pship->GetCluster() != pclusterTarget);
            if (bOnTheWay || AIContains(idleField, pship))
                cRecallable++;
        }
        int     cAvailable = cOnIt + (int)idleHome.size() + (int)camped.size() + cRecallable;
        bHold = (cArmed > 0) && (cAvailable < cArmed);
        if (bHold)
        {
            // Docked ones stay in, and those out in a raided sector of ours go in.
            int cHeld = 0;
            for (size_t i = 0; i < camped.size(); i++)
            {
                IshipIGC*   pship = camped[i];
                bool        bWasHeld = pship->GetStayDocked();
                CommandID   cid = pship->GetCommandID(c_cmdAccepted);
                if ((cid == c_cidGoto) || (cid == c_cidBuild))
                    pship->SetCommand(c_cmdAccepted, NULL, c_cidNone);  // those launch a drone even when it is told to stay docked
                pship->SetStayDocked(true);
                if (!bWasHeld)
                    cHeld++;
            }
            for (size_t i = 0; i < idleHome.size(); )
            {
                IshipIGC*   pship = idleHome[i];
                if ((pship->GetCluster() == NULL) || (s.armedClusters.find(pship->GetCluster()) == s.armedClusters.end()))
                {
                    i++;
                    continue;
                }
                bool            bWasHeld = pship->GetStayDocked();
                IstationIGC*    pstation = AIRefuge(s, pship);
                if (pstation && Order(pship, c_cidGoto, pstation))
                {
                    if (!bWasHeld)
                        cHeld++;
                    pship->SetStayDocked(true);     // after the order, which clears it
                    idleHome.erase(idleHome.begin() + i);
                }
                else
                    i++;
            }
            if (cHeld)
            {
                Log("holds %d more ships docked: %d armed intruders, only %d of ours to take them on", cHeld, cArmed, cAvailable);
                Chat(false, "Outnumbered %d to %d: waiting docked for more ships.", cArmed, cAvailable);
            }
        }

        int cSent = 0;
        while (!bHold && ((cOnIt + cSent) < cWanted) && !idleHome.empty())
        {
            size_t      iBest = 0;
            IshipIGC*   ptargetBest = NULL;
            float       scoreBest = FLT_MAX;
            for (size_t i = 0; i < idleHome.size(); i++)
            {
                float       score;
                IshipIGC*   ptarget = AIPickIntruder(s, idleHome[i], intruders, onIt, score);
                if (AIIsBomber(idleHome[i]->GetBaseHullType()))
                    score += 1.0e8f;
                if (ptarget && (score < scoreBest))
                {
                    scoreBest = score;
                    iBest = i;
                    ptargetBest = ptarget;
                }
            }
            if (ptargetBest == NULL)
                break;

            IshipIGC*   pship = idleHome[iBest];
            idleHome.erase(idleHome.begin() + iBest);
            if (OrderNew(pship, c_cidAttack, ptargetBest))
            {
                onIt[ptargetBest]++;
                cSent++;
            }
        }

        // Not outnumbered: the docked ones go out now, all together.
        int cLaunched = 0;
        for (size_t i = 0; !bHold && (i < camped.size()); i++)
        {
            IshipIGC*   pship = camped[i];
            float       score;
            IshipIGC*   ptarget = AIPickIntruder(s, pship, intruders, onIt, score);
            if (ptarget == NULL)
                continue;
            bool    bWasHeld = pship->GetStayDocked();
            bool    bNew = OrderNew(pship, c_cidAttack, ptarget);
            pship->SetStayDocked(false);    // Order() leaves it alone when the order stays the same
            onIt[ptarget]++;
            if (bNew || bWasHeld)
                cLaunched++;
        }

        int cRecalled = 0;
        if (!bHold && (cArmed >= cGuard) && ((cOnIt + cSent + cLaunched) < cWanted) && pclusterTarget)
        {
            std::vector<std::pair<int, IshipIGC*> > recall;
            for (size_t i = 0; i < s.combat.size(); i++)
            {
                IshipIGC*   pship = s.combat[i];
                ImodelIGC*  ptarget = pship->GetCommandTarget(c_cmdAccepted);
                bool        bOnTheWay = ptarget && (ptarget->GetCluster() == pclusterTarget) &&
                                        (pship->GetCluster() != pclusterTarget);
                if (bOnTheWay || AIContains(idleField, pship))
                    recall.push_back(std::make_pair(AIJumps(s, pship->GetCluster()), pship));
            }
            std::sort(recall.begin(), recall.end());
            for (size_t i = 0; (i < recall.size()) && ((cOnIt + cSent + cLaunched + cRecalled) < cWanted); i++)
            {
                IshipIGC*   pship = recall[i].second;
                float       score;
                IshipIGC*   ptarget = AIPickIntruder(s, pship, intruders, onIt, score);
                if (ptarget && OrderNew(pship, c_cidAttack, ptarget))
                {
                    onIt[ptarget]++;
                    cRecalled++;
                    for (size_t j = 0; j < idleField.size(); j++)
                    {
                        if (idleField[j] == pship)
                        {
                            idleField.erase(idleField.begin() + j);
                            break;
                        }
                    }
                }
            }
            if (cRecalled)
                Log("recalls %d attackers to defend against %d armed intruders", cRecalled, cArmed);
        }

        if (cSent + cRecalled + cLaunched)
        {
            if (cLaunched)
                Log("sends %d ships against %d intruders (%d armed), %d of them launched together from the station",
                    cSent + cRecalled + cLaunched, (int)intruders.size(), cArmed, cLaunched);
            else
                Log("sends %d ships against %d intruders (%d armed)", cSent + cRecalled, (int)intruders.size(), cArmed);
            Chat(false, "Defending: %d ships against %d intruders.", cSent + cRecalled + cLaunched, (int)intruders.size());
        }
    }

    if (attack.pstation == NULL)
    {
        // No enemy stations left: hunt down the ships we can see.
        if (enemies.empty())
        {
            RallyIdleShips(s, idleHome, NULL);
            return;
        }

        int cSent = 0;     // (not while outnumbered at home, see bHold)
        for (size_t i = 0; !bHold && (i < idleField.size()); i++)
        {
            if (OrderNew(idleField[i], c_cidAttack, AINearest(idleField[i], enemies)))
                cSent++;
        }
        for (size_t i = 0; !bHold && (i < idleHome.size()); i++)
        {
            if (OrderNew(idleHome[i], c_cidAttack, AINearest(idleHome[i], enemies)))
                cSent++;
        }
        if (cSent)
            Chat(false, "Hunting %d enemy ships with %d of ours.", (int)enemies.size(), cSent);
        return;
    }

    // No attacks in the first minutes, so both sides can build up; ships still defend.
    if ((m_timeNow - m_timeStart) < 60.0f * (float)o.firstAttackMinutes)
    {
        RallyIdleShips(s, idleHome, pclusterTarget);
        return;
    }

    // Ships already out in the field join the attack straight away.
    int cJoined = 0;
    for (size_t i = 0; i < idleField.size(); i++)
    {
        if (SendAttacker(idleField[i], s, attack))
            cJoined++;
    }
    if (cJoined)
        Log("%d ships in the field head for %s", cJoined, attack.pstation->GetName());

    // A wave from home once enough ships are free, beyond a few fighters that stay to
    // guard it, and not on the heels of the last one, so ships bought together go together.
    if (!bHold && ((int)idleHome.size() >= (cGuard + (int)o.attackGroupSize)) && ((m_timeNow - m_timeLastWave) >= 30.0f))
    {
        int cSent = 0;
        int cBombers = 0;
        int cKeep = cGuard;
        for (size_t i = 0; i < idleHome.size(); i++)
        {
            bool    bBomber = AIIsBomber(idleHome[i]->GetBaseHullType());
            if (!bBomber && (cKeep > 0))
            {
                cKeep--;
                continue;
            }
            if (SendAttacker(idleHome[i], s, attack))
            {
                cSent++;
                if (bBomber)
                    cBombers++;
            }
        }
        if (cSent)
        {
            m_cWaves++;
            m_timeLastWave = m_timeNow;
            Log("wave %d: %d ships (%d bombers) against %s in %s, %d jumps%s",
                m_cWaves, cSent, cBombers, attack.pstation->GetName(), pclusterTarget->GetName(), jumpsBest,
                attack.bSeen ? "" : ", by way of a waypoint outside it");
            Chat(true, "Wave %d: %d ships (%d bombers) attacking %s in %s.",
                 m_cWaves, cSent, cBombers, attack.pstation->GetName(), pclusterTarget->GetName());
        }
    }

    // Ships left at home wait out by the alephs instead of on the launch bays.
    RallyIdleShips(s, idleHome, pclusterTarget);

    // A waypoint no ship took goes now; the others go when their last ship is done with them.
    if (attack.pbuoy)
    {
        if (attack.cBuoy == 0)
            attack.pbuoy->Terminate();
        attack.pbuoy->Release();
    }
}

// Idle ships at home fly out to a spot between their station and the aleph that leads
// toward the enemy, each to its own place there, so they don't sit stacked on the
// launch bays and they meet raiders on the way in.
// Docked ones launch to go there (a Goto order launches a drone at once).
void AICommander::RallyIdleShips(AISurvey& s, std::vector<IshipIGC*>& idleHome, IclusterIGC* pclusterTarget)
{
    if (idleHome.empty() || s.ourStations.empty())
        return;

    // Jumps from each sector to the enemy.
    std::map<IclusterIGC*, int> toEnemy;
    std::vector<IclusterIGC*>   queue;
    if (pclusterTarget)
        queue.push_back(pclusterTarget);
    else
        queue.assign(s.enemyClusters.begin(), s.enemyClusters.end());
    for (size_t i = 0; i < queue.size(); i++)
        toEnemy[queue[i]] = 0;
    for (size_t i = 0; i < queue.size(); i++)
    {
        int jumps = toEnemy[queue[i]] + 1;
        for (WarpLinkIGC* l = queue[i]->GetWarps()->first(); l; l = l->next())
        {
            IwarpIGC*       pwarpDestination = l->data()->GetDestination();
            IclusterIGC*    pcluster = pwarpDestination ? pwarpDestination->GetCluster() : NULL;
            if (pcluster && (toEnemy.find(pcluster) == toEnemy.end()))
            {
                toEnemy[pcluster] = jumps;
                queue.push_back(pcluster);
            }
        }
    }

    ImissionIGC*    pmission = m_pfsMission->GetIGCMission();
    int             cRallied = 0;
    for (size_t i = 0; i < idleHome.size(); i++)
    {
        IshipIGC*       pship = idleHome[i];
        IstationIGC*    pstationDocked = pship->GetStation();
        IclusterIGC*    pcluster = pstationDocked ? pstationDocked->GetCluster() : pship->GetCluster();
        if ((pcluster == NULL) || (pcluster == pclusterTarget) || (pstationDocked && pship->GetStayDocked()) ||
            AIBusy(pship, pclusterTarget))
            continue;

        // Already on its way to a waypoint.
        ImodelIGC*  ptarget = pship->GetCommandTarget(c_cmdAccepted);
        if ((pship->GetCommandID(c_cmdAccepted) == c_cidGoto) && ptarget && (ptarget->GetObjectType() == OT_buoy))
            continue;

        // Our nearest station in its sector (the one it is in, when docked).
        IstationIGC*    pstation = pstationDocked;
        float           d2Best = FLT_MAX;
        for (size_t j = 0; (pstationDocked == NULL) && (j < s.ourStations.size()); j++)
        {
            if (s.ourStations[j]->GetCluster() != pcluster)
                continue;
            float   d2 = (s.ourStations[j]->GetPosition() - pship->GetPosition()).LengthSquared();
            if (d2 < d2Best)
            {
                d2Best = d2;
                pstation = s.ourStations[j];
            }
        }
        if (pstation == NULL)
            continue;

        // The aleph toward the enemy, or the nearest one when we don't know the way.
        const Vector&   position = pstation->GetPosition();
        IwarpIGC*       pwarpBest = NULL;
        int             jumpsBest = INT_MAX;
        float           distanceBest = FLT_MAX;
        for (WarpLinkIGC* l = pcluster->GetWarps()->first(); l; l = l->next())
        {
            IwarpIGC*   pwarp = l->data();
            IwarpIGC*   pwarpOther = pwarp->GetDestination();
            std::map<IclusterIGC*, int>::iterator   it = toEnemy.end();
            if (pwarpOther && pwarpOther->GetCluster())
                it = toEnemy.find(pwarpOther->GetCluster());
            int     jumps = (it == toEnemy.end()) ? INT_MAX : it->second;
            float   distance = (pwarp->GetPosition() - position).Length();
            if ((pwarpBest == NULL) || (jumps < jumpsBest) || ((jumps == jumpsBest) && (distance < distanceBest)))
            {
                pwarpBest = pwarp;
                jumpsBest = jumps;
                distanceBest = distance;
            }
        }

        Vector  direction = pwarpBest ? (pwarpBest->GetPosition() - position) : pstation->GetOrientation().GetForward();
        if (direction.LengthSquared() < 1.0f)
            direction = pstation->GetOrientation().GetForward();
        float   radius = pstation->GetRadius();
        float   range = pwarpBest ? (distanceBest * 0.5f) : (radius + 800.0f);
        if (range < radius + 300.0f)
            range = radius + 300.0f;
        if (range > radius + 800.0f)
            range = radius + 800.0f;

        // Each ship its own spot, the same one every time, within 250m of the rally point.
        unsigned    h = (unsigned)(unsigned short)pship->GetObjectID() * 2654435761u;
        Vector      offset(((float)(h & 0xff) - 127.5f) * (250.0f / 128.0f),
                           ((float)((h >> 8) & 0xff) - 127.5f) * (250.0f / 128.0f),
                           ((float)((h >> 16) & 0xff) - 127.5f) * (250.0f / 128.0f));
        Vector      spot = position + direction.Normalize() * range + offset;
        if ((pstationDocked == NULL) && ((pship->GetPosition() - spot).LengthSquared() < 500.0f * 500.0f))
            continue;

        DataBuoyIGC db;
        db.position = spot;
        db.clusterID = pcluster->GetObjectID();
        db.type = c_buoyWaypoint;
        db.buoyID = pmission->GenerateNewBuoyID();
        db.visible = false;
        IbuoyIGC*   pbuoy = (IbuoyIGC*)(pmission->CreateObject(pmission->GetLastUpdate(), OT_buoy, &db, sizeof(db)));
        if (pbuoy == NULL)
            continue;

        // The waypoint goes when its ship is done with it, or now if the ship didn't take it.
        if (OrderNew(pship, c_cidGoto, pbuoy))
            cRallied++;
        else
            pbuoy->Terminate();
        pbuoy->Release();
    }

    if (cRallied)
        Log("rallies %d idle ships between our stations and the alephs toward the enemy", cRallied);
}

// Send one ship at the attack: fighters take on the guards; bombers, and fighters
// when there are no guards, go for the station, by way of the waypoint while it is unseen.
bool AICommander::SendAttacker(IshipIGC* pship, AISurvey& s, AIAttack& a)
{
    if (!AIIsBomber(pship->GetBaseHullType()) && !a.guards.empty())
        return OrderNew(pship, c_cidAttack, a.guards[a.iGuard++ % a.guards.size()]);

    if (a.bSeen)
        return OrderNew(pship, c_cidAttack, a.pstation);

    if (a.pbuoy == NULL)
    {
        a.pbuoy = AIApproachBuoy(m_pfsMission->GetIGCMission(), s, a.pstation);
        if (a.pbuoy == NULL)
            return false;
    }
    if (!OrderNew(pship, c_cidGoto, a.pbuoy))
        return false;

    a.cBuoy++;
    return true;
}

// Work out which techs lead to combat ships better than the ones we can build, so
// research and stations head that way instead of just buying what's cheapest.
void AICommander::FindWantedTechs(IsideIGC* pside, AISurvey& s)
{
    ImissionIGC*    pmission = m_pfsMission->GetIGCMission();

    // What we have: the side's techs and our stations' own.
    s.ttbmHave = pside->GetTechs();
    for (size_t i = 0; i < s.ourStations.size(); i++)
        s.ttbmHave |= s.ourStations[i]->GetStationType()->GetLocalTechs();

    // What we could get, through any chain of research and stations.
    s.ttbmReach = s.ttbmHave;
    for (int pass = 0; pass < 20; pass++)
    {
        TechTreeBitMask before = s.ttbmReach;
        for (DevelopmentLinkIGC* l = pmission->GetDevelopments()->first(); l; l = l->next())
        {
            IdevelopmentIGC*    pd = l->data();
            if ((pd->GetGroupID() >= 0) && AISubset(pd->GetRequiredTechs(), s.ttbmReach))
                s.ttbmReach |= pd->GetEffectTechs();
        }
        for (StationTypeLinkIGC* l = pmission->GetStationTypes()->first(); l; l = l->next())
        {
            IstationTypeIGC*    pst = l->data();
            if ((pst->GetGroupID() >= 0) && AISubset(pst->GetRequiredTechs(), s.ttbmReach))
            {
                s.ttbmReach |= pst->GetEffectTechs();
                s.ttbmReach |= pst->GetLocalTechs();
            }
        }
        if (s.ttbmReach == before)
            break;
    }

    // The dearest combat ship of each class we can build now.
    Money   costBest[2] = { 0, 0 };     // fighters, bombers
    for (HullTypeLinkIGC* l = pmission->GetHullTypes()->first(); l; l = l->next())
    {
        IhullTypeIGC*   pht = l->data();
        if (!AIIsCombatHull(pht) || (AIStationFor(s, pht) == NULL))
            continue;
        int     c = AIIsBomber(pht) ? 1 : 0;
        Money   cost = AIShipCost(pht);
        if (cost > costBest[c])
            costBest[c] = cost;
    }

    // Techs missing for better ships we could reach...
    TechTreeBitMask wanted;
    wanted.ClearAll();
    for (HullTypeLinkIGC* l = pmission->GetHullTypes()->first(); l; l = l->next())
    {
        IhullTypeIGC*   pht = l->data();
        if (!AIIsCombatHull(pht) || (AIStationFor(s, pht) != NULL) ||
            !AISubset(pht->GetRequiredTechs(), s.ttbmReach))
            continue;

        int c = AIIsBomber(pht) ? 1 : 0;
        if (((c == 1) && (costBest[1] == 0)) || (AIShipCost(pht) > costBest[c]))
        {
            TechTreeBitMask missing = pht->GetRequiredTechs();
            missing -= s.ttbmHave;
            wanted |= missing;
        }
    }

    // ... and, working back, what the research and stations that give them need.
    for (int pass = 0; pass < 10; pass++)
    {
        TechTreeBitMask before = wanted;
        for (DevelopmentLinkIGC* l = pmission->GetDevelopments()->first(); l; l = l->next())
        {
            IdevelopmentIGC*    pd = l->data();
            if ((pd->GetGroupID() >= 0) && AISubset(pd->GetRequiredTechs(), s.ttbmReach) &&
                AIOverlaps(pd->GetEffectTechs(), wanted))
            {
                TechTreeBitMask missing = pd->GetRequiredTechs();
                missing -= s.ttbmHave;
                wanted |= missing;
            }
        }
        for (StationTypeLinkIGC* l = pmission->GetStationTypes()->first(); l; l = l->next())
        {
            IstationTypeIGC*    pst = l->data();
            if ((pst->GetGroupID() >= 0) && AISubset(pst->GetRequiredTechs(), s.ttbmReach) &&
                (AIOverlaps(pst->GetEffectTechs(), wanted) || AIOverlaps(pst->GetLocalTechs(), wanted)))
            {
                TechTreeBitMask missing = pst->GetRequiredTechs();
                missing -= s.ttbmHave;
                wanted |= missing;
            }
        }
        if (wanted == before)
            break;
    }

    s.ttbmWanted = wanted;
}

// At the start, log every combat hull in the core and how far away it is; after
// that, log the ships we can build whenever that list changes.
void AICommander::LogHulls(AISurvey& s, bool bFirst)
{
    ImissionIGC*    pmission = m_pfsMission->GetIGCMission();

    char    szHulls[sizeof(m_szHulls)];
    szHulls[0] = '\0';
    int     cOut = 0;
    for (HullTypeLinkIGC* l = pmission->GetHullTypes()->first(); l; l = l->next())
    {
        IhullTypeIGC*   pht = l->data();
        if (!AIIsCombatHull(pht))
            continue;

        bool    bNow = (AIStationFor(s, pht) != NULL);
        if (bNow)
        {
            if (szHulls[0])
                strncat_s(szHulls, sizeof(szHulls), ", ", _TRUNCATE);
            strncat_s(szHulls, sizeof(szHulls), pht->GetName(), _TRUNCATE);
        }

        if (!bFirst)
            continue;
        if (!AISubset(pht->GetRequiredTechs(), s.ttbmReach))
        {
            cOut++;
            continue;
        }

        TechTreeBitMask missing = pht->GetRequiredTechs();
        missing -= s.ttbmHave;
        Log("hull %s: %s, costs %d, %d hit points, %d guns, %s", pht->GetName(), AIIsBomber(pht) ? "bomber" : "fighter", AIShipCost(pht),
            (int)pht->GetHitPoints(), (int)pht->GetMaxWeapons(),
            bNow ? "buildable now"
                 : (missing.GetAllZero() ? "have the techs, not sold at our stations"
                                         : (AIOverlaps(missing, s.ttbmWanted) ? "a goal" : "reachable")));
    }
    if (bFirst)
        Log("%d combat hulls are out of reach (other factions, or techs we can't get)", cOut);

    if (strcmp(szHulls, m_szHulls) != 0)
    {
        strcpy_s(m_szHulls, sizeof(m_szHulls), szHulls);
        Log("ships we can build: %s", szHulls[0] ? szHulls : "none");
    }
}

// Log the research we can buy whenever that list changes (* leads to better ships),
// and the research we're saving for whenever that changes.
void AICommander::LogResearch(AISurvey& s, std::vector<IbucketIGC*>& buckets, IbucketIGC* pbucketNext, bool bWanted)
{
    char    szResearch[sizeof(m_szResearch)];
    szResearch[0] = '\0';
    for (size_t i = 0; i < buckets.size(); i++)
    {
        char    bfr[c_cbName + 20];
        sprintf_s(bfr, sizeof(bfr), "%s%s %d%s", i ? ", " : "", buckets[i]->GetBuyable()->GetName(),
                  buckets[i]->GetPrice(), AIOverlaps(buckets[i]->GetBuyable()->GetEffectTechs(), s.ttbmWanted) ? "*" : "");
        strncat_s(szResearch, sizeof(szResearch), bfr, _TRUNCATE);
    }
    if (strcmp(szResearch, m_szResearch) != 0)
    {
        strcpy_s(m_szResearch, sizeof(m_szResearch), szResearch);
        Log("research we can buy: %s", szResearch[0] ? szResearch : "none");
    }

    ObjectID    idNext = pbucketNext ? pbucketNext->GetObjectID() : NA;
    if (idNext != m_idResearchNext)
    {
        m_idResearchNext = idNext;
        if (pbucketNext)
            Log("next research: %s for %d%s, %d set aside", pbucketNext->GetBuyable()->GetName(),
                pbucketNext->GetPrice(), bWanted ? ", toward better ships" : "", m_potResearch);
    }
}

// Give a drone an order the way a commander would. Orders it can't take turn
// into a plain Goto to the same target.
bool AICommander::Order(IshipIGC* pship, CommandID cid, ImodelIGC* ptarget)
{
    if (ptarget == NULL)
        return false;

    if ((pship->GetCommandID(c_cmdAccepted) == cid) && (pship->GetCommandTarget(c_cmdAccepted) == ptarget))
        return true;

    if (!pship->LegalCommand(cid, ptarget))
    {
        if ((cid == c_cidGoto) || !pship->LegalCommand(c_cidGoto, ptarget))
            return false;
        cid = c_cidGoto;
        if ((pship->GetCommandID(c_cmdAccepted) == cid) && (pship->GetCommandTarget(c_cmdAccepted) == ptarget))
            return true;
    }

    pship->SetStayDocked(false);
    if (pship->GetPilotType() != c_ptMiner)    // miners keep the stock checks, so they still run from raiders
        pship->SetRunawayCheckCooldown(120.0f);
    pship->SetCommand(c_cmdAccepted, ptarget, cid);
    return true;
}

// Like Order(), but true only when the ship's order changed, for counting ships sent.
bool AICommander::OrderNew(IshipIGC* pship, CommandID cid, ImodelIGC* ptarget)
{
    CommandID   cidOld = pship->GetCommandID(c_cmdAccepted);
    ImodelIGC*  ptargetOld = pship->GetCommandTarget(c_cmdAccepted);
    if (!Order(pship, cid, ptarget))
        return false;

    return (pship->GetCommandID(c_cmdAccepted) != cidOld) || (pship->GetCommandTarget(c_cmdAccepted) != ptargetOld);
}
