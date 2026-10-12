/**
 * @file        net/online_stats.h
 * @brief       Online identity, ranked results and the leaderboard, on top of
 *              the online lobby (rex/net/online.h).
 *
 * Every install gets an identity from the lobby (POST /v1/register: a player
 * id and a secret key, kept in <data folder>/online_identity.json and sent in
 * the lobby hello). When both players of a lobby match have one, the lobby
 * opens a match record; the game reports each finished battle (winner side
 * and characters) and the lobby counts it once both reports agree (rating in
 * Ranked Match rooms, games / wins / losses / favourite character always).
 *
 * The game's own Xbox LIVE leaderboard / stats calls (XUserReadStats,
 * XamUserCreateStatsEnumerator) are answered from the same data.
 *
 * Design: C:/rex/_online_tests/RANKED_DESIGN.md, lobby side RANKED.md.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rex::net::online::stats {

/// Folder for online_identity.json (the user data folder). Call at startup,
/// before the lobby is used.
void SetDataFolder(const std::filesystem::path& folder);

struct CharacterCount {
  int character = -1;
  std::string name;
  int games = 0;
  int wins = 0;
};

struct PlayerStats {
  std::string id;
  std::string name;
  int rank = 0;       // ranked position, 0 = not ranked yet
  int power = 5000;   // the game's Power Level (Ranked Match)
  int peak = 5000;
  std::string grade = "F";  // Z S A B C D E F
  int grade_index = 0;      // the game's grade: 0 = F .. 7 = Z
  int country = 0;
  int ranked_games = 0, ranked_wins = 0, ranked_losses = 0, ranked_draws = 0;
  int games = 0, wins = 0, losses = 0, draws = 0, abandons = 0;
  int fav_char = -1;  // most played character (0..20), -1 none yet
  std::string fav_name;
  int fav_games = 0;
  std::vector<CharacterCount> characters;  // only from FetchPlayer / Me
};

struct LeaderboardPage {
  bool ok = false;
  std::string error;  // why not ok
  std::string mode;   // "ranked" (by rating) or "all" (by online battles)
  int total = 0;
  int offset = 0;
  std::vector<PlayerStats> rows;  // rows[i].rank = position in this mode
};

/// The character's name (0 Goku .. 20 Bardock), "" for others.
const char* CharacterName(int character);

/// Whether this install has an identity for the configured lobby, and the
/// lobby accepted it (after the welcome).
bool HasIdentity();
/// A short line about the identity / connection, for menus.
std::string IdentityStatus();

/// The latest own numbers (from the welcome and every battle result). False
/// without an identity.
bool Me(PlayerStats* out);

/// Leaderboard page from the lobby (HTTP, blocking up to timeout_ms).
/// mode "ranked" / "all"; around_me centres the page on this player.
LeaderboardPage FetchLeaderboard(const std::string& mode, int offset, int limit,
                                 bool around_me = false, int country = -1,
                                 int timeout_ms = 3000);
/// One player's card with the per-character counts (HTTP, blocking).
bool FetchPlayer(const std::string& id, PlayerStats* out, int timeout_ms = 3000);

// --- the current lobby match -------------------------------------------------

struct MatchInfo {
  std::string id;
  bool ranked = false;
  int side = 0;  // 0 host (player 1), 1 guest (player 2)
  PlayerStats opponent;
  int battles = 0;  // battles reported so far
};
/// True while a lobby match with a match record is on.
bool CurrentMatch(MatchInfo* out);

/// One finished battle, as the game's results screen saw it (side 0 = host =
/// player 1 on both consoles).
struct BattleReport {
  int winner = 2;           // 0 host, 1 guest, 2 draw
  int chars[2] = {-1, -1};  // 0..20
  int played = 0;           // rounds played
  int won[2] = {0, 0};      // rounds won per side
  int ko5[2] = {0, 0};      // rounds per side with the game's code 5 (Power Level bonus)
  int country = -1;         // own location 0..7 (the game's), -1 unknown
  int game_delta[2] = {0, 0};  // the Power Level change the game computed (for the log)
};
/// A battle of the current match ended (both consoles report it).
void ReportBattle(const BattleReport& report);
/// The game marks a battle of an online session as running (true at its
/// start, false once reported). A session that ends while one runs reports
/// it as abandoned by the opponent.
void SetBattleInProgress(bool running, int host_char = -1, int guest_char = -1);

struct BattleResult {
  std::string match;
  int seq = 0;
  std::string state;  // recorded / disputed / void
  bool ranked = false;
  bool rated = false;
  int winner = -1;  // 0 host, 1 guest, 2 draw
  int side = 0;     // own side
  int delta = 0;    // own rating change
  PlayerStats you;
};
/// The latest settled battle of this player. `serial` grows with every new one.
bool LastBattleResult(BattleResult* out, uint64_t* serial);

// --- for the Xbox LIVE layer (XUserReadStats, stats enumerators, gamer cards) --

/// The profile XUID the game uses for this player (rows with it are "you").
void SetLocalXuid(uint64_t xuid);
uint64_t LocalXuid();
/// A stable XUID the game sees for a lobby player id (the local player gets
/// the profile XUID).
uint64_t XuidForPlayer(const std::string& id);
/// The lobby player behind an XUID handed out by XuidForPlayer.
bool PlayerForXuid(uint64_t xuid, std::string* id);

/// Who the next XUserReadStats on this thread is about: the game sets it
/// before reading a player record (0 = this player, 1 = the opponent).
void SetReadTarget(int target);
int ReadTarget();
/// The card for an XUserReadStats row: this player or the current opponent.
PlayerStats CardForRead(int target, bool* known);

/// The last leaderboard page the game's own leaderboard screen got (the
/// stats enumerator remembers it), and how old it is.
void RememberBoardPage(const LeaderboardPage& page);
bool LastBoardPage(LeaderboardPage* out, int64_t* age_ms);

/// A gamer card the game asked to show (XamShowGamerCardUIForXUID); the UI
/// takes it once.
void RequestCard(uint64_t xuid);
bool TakeCardRequest(std::string* id, PlayerStats* card);

}  // namespace rex::net::online::stats
