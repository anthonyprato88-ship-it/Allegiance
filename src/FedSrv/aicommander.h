/*-------------------------------------------------------------------------
 * aicommander.h
 *
 * An AI commander for a side that has no human commander. It researches,
 * buys stations and miners, buys combat ships flown by the drone AI, and
 * sends them to mine, build, defend and attack.
 *
 * Turn it on per game in the server's registry key: AIvsAI<n> = 1 puts an
 * AI commander on every side of game <n>, or AISides<n> is a bit mask of
 * AI sides (bit 0 = the first side) for humans against the AI. See
 * aicommander.cpp for the tuning values.
 *-----------------------------------------------------------------------*/
#ifndef _AICOMMANDER_H_
#define _AICOMMANDER_H_

class CFSMission;
struct AISurvey;
struct AIAttack;

// Tuning values, read from the server's registry key (HKCU first, then HKLM).
struct AIOptions
{
    DWORD   thinkSeconds;       // AIThinkSeconds:     seconds between decisions
    DWORD   maxMiners;          // AIMaxMiners:        miners to keep
    DWORD   maxConstructors;    // AIMaxConstructors:  constructors in flight at once
    DWORD   maxStations;        // AIMaxStations:      stations to own, counting ones being built
    DWORD   maxShips;           // AIMaxShips:         combat ships per side
    DWORD   maxResearch;        // AIMaxResearch:      research projects at once
    DWORD   attackGroupSize;    // AIAttackGroupSize:  least ships in a wave; half as many more stay home on guard
    DWORD   shipPercent;        // AIShipPercent:      share of income set aside for combat ships
    DWORD   researchPercent;    // AIResearchPercent:  share of income set aside for research
    DWORD   bomberPercent;      // AIBomberPercent:    share of combat ships that are bombers
    DWORD   minShipCost;        // AIMinShipCost:      least a combat ship costs
    DWORD   shipCostPercent;    // AIShipCostPercent:  combat ship cost, percent of hull plus parts
    DWORD   shootSkill;         // AIShootSkill:       combat pilot skills, percent
    DWORD   moveSkill;          // AIMoveSkill
    DWORD   bravery;            // AIBravery
    DWORD   chat;               // AIChat:             0 silent, 1 team chat, 2 chat to everyone
    DWORD   gameMinutes;        // AIGameMinutes:      match time limit, 0 for none
    DWORD   autoRestart;        // AIAutoRestart:      start a new match when one ends
    DWORD   startSeconds;       // AIStartSeconds:     countdown before the match starts
    DWORD   firstAttackMinutes; // AIFirstAttackMinutes: no waves before this, so both sides can build up
    DWORD   initialMiners;      // AIInitialMiners:    miners each side starts with
    DWORD   showMap;            // AIShowMap:          1 shows every side the whole map (all alephs)
    char    logFile[MAX_PATH];  // AILogFile:          defaults to AICommander.log next to AllSrv.exe
};

const AIOptions&    AIGetOptions(bool bReload = false);
void                AILog(const char* format, ...);
void                AIApplyMissionParams(MissionParams* pmp);
unsigned            AIGetGameSides(HKEY hKey, int iGame, int cTeams);

class AICommander
{
public:
    AICommander(CFSMission* pfsMission, SideID sideID);
    ~AICommander();

    void            Tick(Time now);

private:
    IsideIGC*       GetSide() const;
    void            Log(const char* format, ...);
    void            Chat(bool bForce, const char* format, ...);
    void            Report(AISurvey& survey);

    void            TakeSurvey(IsideIGC* pside, AISurvey& survey);
    void            OrderMiners(AISurvey& survey);
    void            OrderBuilders(AISurvey& survey);
    void            Spend(IsideIGC* pside, AISurvey& survey);
    void            OrderCombatShips(IsideIGC* pside, AISurvey& survey);
    bool            SendAttacker(IshipIGC* pship, AISurvey& survey, AIAttack& attack);
    void            FindWantedTechs(IsideIGC* pside, AISurvey& survey);
    void            LogHulls(AISurvey& survey, bool bFirst);
    void            LogResearch(AISurvey& survey, std::vector<IbucketIGC*>& buckets, IbucketIGC* pbucketNext, bool bWanted);
    void            RallyIdleShips(AISurvey& survey, std::vector<IshipIGC*>& idleHome, IclusterIGC* pclusterTarget);

    Money           BuyBucket(IsideIGC* pside, IbucketIGC* pbucket, Money reserve);
    bool            BuyMiner(IsideIGC* pside, AISurvey& survey, IbucketIGC* pbucket, Money reserve);
    bool            BuyResearch(IsideIGC* pside, AISurvey& survey, IbucketIGC* pbucket, bool bWanted, Money reserve);
    int             BuyShipsFromPot(IsideIGC* pside, AISurvey& survey, Money reserve, int cWanted);
    void            ClampPots(void);
    int             BuyCombatShips(IsideIGC* pside, AISurvey& survey, Money reserve, int cWanted);
    int             BuyCombatShip(IsideIGC* pside, AISurvey& survey, bool bBomber, Money budget);
    IasteroidIGC*   FindMineRock(AISurvey& survey, IshipIGC* pship);
    IasteroidIGC*   FindBuildRock(AISurvey& survey, AsteroidAbilityBitMask aabm);
    bool            Order(IshipIGC* pship, CommandID cid, ImodelIGC* ptarget);
    bool            OrderNew(IshipIGC* pship, CommandID cid, ImodelIGC* ptarget);

    CFSMission*     m_pfsMission;
    SideID          m_sideID;
    bool            m_bStarted;
    char            m_szSideName[c_cbName];
    Time            m_timeNow;
    Time            m_timeStart;
    Time            m_timeNextThink;
    Time            m_timeNextReport;
    Time            m_timeLastChat;
    Time            m_timeLastWave;

    Money           m_moneySpent;
    Money           m_moneyResearch;
    Money           m_moneyEarned;          // money plus spending at the last spend, -1 before the first
    Money           m_moneyEarnedReport;    // the same at the last report
    Money           m_potShips;             // income set aside for combat ships
    Money           m_potResearch;          // income set aside for research
    int             m_cMinersBought;
    int             m_cStationsBought;
    int             m_cResearchBought;
    int             m_cShipsBought;
    int             m_cWaves;
    int             m_cOurStations;     // stations at the last think, -1 before the first
    int             m_cEnemyStations;
    char            m_szHulls[400];     // the last list of ships we can build that was logged
    char            m_szResearch[600];  // the last list of research we can buy that was logged
    char            m_szStations[400];  // the last list of stations we can buy that was logged
    ObjectID        m_idResearchNext;   // the research we were saving for at the last spend, NA for none
    std::map<SectorID, Time>    m_timeThreat;   // when armed enemy ships were last seen in each sector
    std::set<ObjectID>          m_minersPulledBack; // miners we sent home from raiders, until they dock or go back out
};

#endif
