/**
 * @file        system/net/online_stats.cpp
 * @brief       Online identity, battle reports and leaderboard queries. See
 *              include/rex/net/online_stats.h.
 *
 * Threads: lobby messages arrive on the lobby worker (online.cpp); the game
 * and the UI call the public functions from their own threads. Everything
 * shared is behind one mutex; HTTP requests never run under it.
 */

#include <rex/net/online_stats.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/net/online.h>

#include "http_client.h"
#include "online_stats_internal.h"

namespace rex::net::online::stats {
namespace {

using json = nlohmann::json;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

constexpr const char* kCharacters[21] = {
    "Goku",       "Kid Gohan",  "Teen Gohan", "Vegeta",        "Krillin", "Trunks",   "Piccolo",
    "Tien",       "Yamcha",     "Raditz",     "Nappa",         "Captain Ginyu", "Recoome",
    "Frieza",     "Android 16", "Android 17", "Android 18",    "Cell",    "Saibamen", "Broly",
    "Bardock"};

int JInt(const json& j, const char* key, int fallback = 0) {
  if (j.is_object()) {
    auto it = j.find(key);
    if (it != j.end() && it->is_number()) {
      return it->get<int>();
    }
  }
  return fallback;
}

std::string JStr(const json& j, const char* key) {
  if (j.is_object()) {
    auto it = j.find(key);
    if (it != j.end() && it->is_string()) {
      return it->get<std::string>();
    }
  }
  return {};
}

PlayerStats ParsePlayer(const json& j) {
  PlayerStats p;
  if (!j.is_object()) {
    return p;
  }
  p.id = JStr(j, "id");
  p.name = JStr(j, "name");
  p.rank = JInt(j, "rank", 0);
  p.power = JInt(j, "power", 5000);
  p.peak = JInt(j, "peak", p.power);
  p.grade = JStr(j, "grade");
  if (p.grade.empty()) {
    p.grade = "F";
  }
  p.grade_index = std::clamp(JInt(j, "grade_index", 0), 0, 7);
  p.country = JInt(j, "country", 0);
  if (j.contains("ranked") && j["ranked"].is_object()) {
    const json& r = j["ranked"];
    p.ranked_games = JInt(r, "games");
    p.ranked_wins = JInt(r, "wins");
    p.ranked_losses = JInt(r, "losses");
    p.ranked_draws = JInt(r, "draws");
  }
  p.games = JInt(j, "games");
  p.wins = JInt(j, "wins");
  p.losses = JInt(j, "losses");
  p.draws = JInt(j, "draws");
  p.abandons = JInt(j, "abandons");
  if (j.contains("fav") && j["fav"].is_object()) {
    p.fav_char = JInt(j["fav"], "char", -1);
    p.fav_name = JStr(j["fav"], "name");
    p.fav_games = JInt(j["fav"], "games");
  }
  if (j.contains("characters") && j["characters"].is_array()) {
    for (const json& c : j["characters"]) {
      CharacterCount count;
      count.character = JInt(c, "char", -1);
      count.name = JStr(c, "name");
      count.games = JInt(c, "games");
      count.wins = JInt(c, "wins");
      p.characters.push_back(count);
    }
  }
  return p;
}

std::string UrlEncode(const std::string& text) {
  std::string out;
  for (unsigned char c : text) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += char(c);
    } else {
      out += fmt::format("%{:02X}", c);
    }
  }
  return out;
}

class Stats {
 public:
  static Stats& Get() {
    static Stats* stats = new Stats();
    return *stats;
  }

  void SetDataFolder(const std::filesystem::path& folder) {
    std::lock_guard<std::mutex> lock(mutex_);
    data_folder_ = folder;
  }

  // --- identity ---
  void OnLobbyMode(const std::string& ws_url) {
    const std::string base = LobbyRestBase(ws_url);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      rest_base_ = base;
      if (base.empty()) {
        return;
      }
      LoadIdentityLocked();
      if (!id_.empty()) {
        REXSYS_INFO("[Ranked] identity {} for {}", id_, base);
        return;
      }
      registering_ = true;
    }
    std::thread([this] { Register(); }).detach();
  }

  json HelloAuth(int wait_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (id_.empty() && !registering_ && !rest_base_.empty()) {
      // No identity yet (the lobby was unreachable before): try again now that it answers.
      registering_ = true;
      std::thread([this] { Register(); }).detach();
    }
    cv_.wait_for(lock, std::chrono::milliseconds(wait_ms), [this] { return !registering_; });
    if (id_.empty() || key_.empty()) {
      return json();
    }
    return json{{"id", id_}, {"key", key_}};
  }

  bool HasIdentity() {
    std::lock_guard<std::mutex> lock(mutex_);
    return !id_.empty() && accepted_;
  }

  std::string IdentityStatus() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (rest_base_.empty()) {
      return "Online lobby off";
    }
    if (registering_) {
      return "Getting an online identity...";
    }
    if (id_.empty()) {
      return register_error_.empty() ? "No online identity yet"
                                     : "No online identity: " + register_error_;
    }
    if (!accepted_) {
      return "Online identity ready (not connected to the lobby)";
    }
    return fmt::format("Online as {}: Power Level {} (grade {}), {} W / {} L", me_.name,
                       me_.power, me_.grade, me_.wins, me_.losses);
  }

  bool Me(PlayerStats* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (id_.empty() || !have_me_) {
      return false;
    }
    *out = me_;
    return true;
  }

  // --- lobby messages (worker thread) ---
  void OnLobbyMessage(const std::string& type, const json& m) {
    if (type == "welcome") {
      if (!m.contains("player")) {
        return;  // hello without auth
      }
      std::string reregister;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (m["player"].is_object()) {
          me_ = ParsePlayer(m["player"]);
          have_me_ = true;
          accepted_ = true;
          REXSYS_WARN("[Ranked] welcome: {} power {} grade {} rank {} games {} ({}W {}L)",
                      me_.name, me_.power, me_.grade, me_.rank, me_.games, me_.wins, me_.losses);
        } else {
          accepted_ = false;
          REXSYS_WARN("[Ranked] the lobby doesn't know identity {} ({}): getting a new one", id_,
                      JStr(m, "auth_error"));
          id_.clear();
          key_.clear();
          try {
            SaveIdentityLocked();
          } catch (const std::exception& e) {
            REXSYS_WARN("[Ranked] couldn't save the online identity: {}", e.what());
          }
          if (!rest_base_.empty() && !registering_) {
            registering_ = true;
            reregister = rest_base_;
          }
        }
      }
      if (!reregister.empty()) {
        std::thread([this] { Register(); }).detach();
      }
    } else if (type == "authed") {
      // An identity added after hello (online.cpp: t:auth after a refused ranked create / join).
      std::lock_guard<std::mutex> lock(mutex_);
      if (m.contains("player") && m["player"].is_object()) {
        me_ = ParsePlayer(m["player"]);
        have_me_ = true;
        accepted_ = true;
        REXSYS_WARN("[Ranked] authed: {} power {} grade {} rank {}", me_.name, me_.power, me_.grade,
                    me_.rank);
      }
    } else if (type == "joined" || type == "peer_joined") {
      std::lock_guard<std::mutex> lock(mutex_);
      match_ = MatchInfo{};
      have_match_ = false;
      if (m.contains("match") && m["match"].is_object()) {
        const json& x = m["match"];
        match_.id = JStr(x, "id");
        match_.ranked = x.contains("ranked") && x["ranked"].is_boolean() && x["ranked"].get<bool>();
        match_.side = JInt(x, "side", type == "joined" ? 1 : 0);
        match_.opponent = ParsePlayer(x.contains("opponent") ? x["opponent"] : json());
        have_match_ = !match_.id.empty();
        REXSYS_WARN("[Ranked] match {} ({}), side {}, opponent {} power {}", match_.id,
                    match_.ranked ? "ranked" : "player match", match_.side,
                    match_.opponent.name, match_.opponent.power);
        if (!match_.opponent.id.empty()) {
          XuidForPlayerLocked(match_.opponent.id);
        }
      }
    } else if (type == "result_ack") {
      REXSYS_INFO("[Ranked] result_ack match {} battle {}: {}", JStr(m, "match"),
                  JInt(m, "seq"), JStr(m, "status"));
    } else if (type == "battle_result") {
      BattleResult r;
      r.match = JStr(m, "match");
      r.seq = JInt(m, "seq");
      r.state = JStr(m, "state");
      r.ranked = m.contains("ranked") && m["ranked"].is_boolean() && m["ranked"].get<bool>();
      r.rated = m.contains("rated") && m["rated"].is_boolean() && m["rated"].get<bool>();
      r.winner = m.contains("winner") && m["winner"].is_number() ? m["winner"].get<int>() : -1;
      r.side = JInt(m, "side");
      if (m.contains("delta") && m["delta"].is_array() && m["delta"].size() == 2) {
        r.delta = m["delta"][r.side == 1 ? 1 : 0].get<int>();
      }
      r.you = ParsePlayer(m.contains("you") ? m["you"] : json());
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!r.you.id.empty()) {
          me_ = r.you;
          have_me_ = true;
        }
        last_result_ = r;
        ++result_serial_;
      }
      REXSYS_WARN("[Ranked] battle {} of {}: {} winner={} rated={} delta={:+} -> power {} grade {} "
                  "games {} ({}W {}L {}D) fav {}",
                  r.seq, r.match, r.state, r.winner, r.rated ? 1 : 0, r.delta, r.you.power,
                  r.you.grade, r.you.games, r.you.wins, r.you.losses, r.you.draws,
                  r.you.fav_name.empty() ? "-" : r.you.fav_name);
      if (r.state == "recorded") {
        const bool won = r.winner == r.side;
        const std::string outcome = r.winner == 2 ? "Draw" : won ? "Win" : "Loss";
        if (r.ranked && r.rated) {
          PostOnlineNotice(fmt::format("Ranked {}: Power Level {} ({:+}), Grade {}  -  {} W / {} L",
                                       outcome, r.you.power, r.delta, r.you.grade,
                                       r.you.ranked_wins, r.you.ranked_losses),
                           7.0);
        } else {
          PostOnlineNotice(fmt::format("Online record: {} W / {} L ({} battles)", r.you.wins,
                                       r.you.losses, r.you.games),
                           5.0);
        }
      } else if (r.state == "disputed") {
        PostOnlineNotice("This battle's results didn't match between the players: not counted", 6.0);
      }
    } else if (type == "me") {
      std::lock_guard<std::mutex> lock(mutex_);
      if (m.contains("player") && m["player"].is_object()) {
        me_ = ParsePlayer(m["player"]);
        have_me_ = true;
      }
    } else if (type == "error") {
      const std::string re = JStr(m, "re");
      const std::string code = JStr(m, "code");
      if (re == "result") {
        REXSYS_WARN("[Ranked] result refused: {}", code);
      } else if (re == "join" && code == "same_player") {
        PostOnlineNotice("That Ranked room is your own", 5.0);
      }
    }
  }

  void OnLobbyClosed() {
    std::lock_guard<std::mutex> lock(mutex_);
    accepted_ = false;
  }

  void OnSessionEnded() {
    std::string abandon;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (have_match_ && battle_running_) {
        const int seq = ++match_.battles;
        json m{{"t", "result"}, {"match", match_.id}, {"seq", seq}, {"abandon", true}};
        if (battle_chars_[0] >= 0 && battle_chars_[1] >= 0) {
          m["chars"] = json::array({battle_chars_[0], battle_chars_[1]});
        }
        abandon = m.dump();
        REXSYS_WARN("[Ranked] session ended during battle {} of {}: reported as abandoned", seq,
                    match_.id);
      }
      battle_running_ = false;
    }
    if (!abandon.empty()) {
      SendLobbyText(std::move(abandon));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (have_match_) {
      REXSYS_INFO("[Ranked] match {} over ({} battles reported)", match_.id, match_.battles);
    }
    have_match_ = false;
    match_ = MatchInfo{};
  }

  bool CurrentMatch(MatchInfo* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!have_match_) {
      return false;
    }
    *out = match_;
    return true;
  }

  void ReportBattle(const BattleReport& b) {
    std::string text;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      battle_running_ = false;
      if (!have_match_) {
        REXSYS_INFO("[Ranked] battle over (winner {}, chars {} vs {}): no match record, not reported",
                    b.winner, b.chars[0], b.chars[1]);
        return;
      }
      const int seq = ++match_.battles;
      json m{{"t", "result"},
             {"match", match_.id},
             {"seq", seq},
             {"winner", b.winner},
             {"chars", json::array({b.chars[0], b.chars[1]})},
             {"played", b.played},
             {"won", json::array({b.won[0], b.won[1]})},
             {"ko5", json::array({b.ko5[0], b.ko5[1]})}};
      if (b.country >= 0 && b.country <= 7) {
        m["country"] = b.country;
      }
      text = m.dump();
      REXSYS_WARN("[Ranked] report battle {} of {}: winner {} chars {} vs {} rounds {} won {}-{} "
                  "ko5 {}-{} (game power change {:+} / {:+})",
                  seq, match_.id, b.winner, b.chars[0], b.chars[1], b.played, b.won[0], b.won[1],
                  b.ko5[0], b.ko5[1], b.game_delta[0], b.game_delta[1]);
    }
    SendLobbyText(std::move(text));
  }

  void SetBattleInProgress(bool running, int host_char, int guest_char) {
    std::lock_guard<std::mutex> lock(mutex_);
    battle_chars_[0] = host_char;
    battle_chars_[1] = guest_char;
    if (running != battle_running_) {
      REXSYS_INFO("[Ranked] battle {}", running ? "started" : "no longer running");
    }
    battle_running_ = running;
  }

  // --- Xbox LIVE layer ---
  void SetLocalXuid(uint64_t xuid) {
    std::lock_guard<std::mutex> lock(mutex_);
    local_xuid_ = xuid;
  }
  uint64_t LocalXuid() {
    std::lock_guard<std::mutex> lock(mutex_);
    return local_xuid_;
  }
  uint64_t XuidForPlayer(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return XuidForPlayerLocked(id);
  }
  bool PlayerForXuid(uint64_t xuid, std::string* id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (local_xuid_ && xuid == local_xuid_ && !id_.empty()) {
      *id = id_;
      return true;
    }
    auto it = xuid_players_.find(xuid);
    if (it != xuid_players_.end()) {
      *id = it->second;
      return true;
    }
    // Some screens keep only the low 32 bits of a row's XUID (the leaderboard's gamer card).
    if ((xuid >> 32) == 0) {
      for (const auto& [full, player] : xuid_players_) {
        if (uint32_t(full) == uint32_t(xuid)) {
          *id = player;
          return true;
        }
      }
    }
    return false;
  }
  PlayerStats CardForRead(int target, bool* known) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (target == 1) {
      *known = have_match_ && !match_.opponent.id.empty();
      return *known ? match_.opponent : PlayerStats{};
    }
    *known = have_me_;
    return have_me_ ? me_ : PlayerStats{};
  }
  void RememberBoardPage(const LeaderboardPage& page) {
    std::lock_guard<std::mutex> lock(mutex_);
    board_page_ = page;
    board_page_ms_ = NowMs();
  }
  bool LastBoardPage(LeaderboardPage* out, int64_t* age_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!board_page_ms_) {
      return false;
    }
    *out = board_page_;
    *age_ms = NowMs() - board_page_ms_;
    return true;
  }

  void RequestCard(uint64_t xuid) {
    std::lock_guard<std::mutex> lock(mutex_);
    card_xuid_ = xuid;
    card_pending_ = true;
  }
  bool TakeCardRequest(std::string* id, PlayerStats* card) {
    uint64_t xuid = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!card_pending_) {
        return false;
      }
      card_pending_ = false;
      xuid = card_xuid_;
    }
    std::string player;
    if (!PlayerForXuid(xuid, &player)) {
      // A session member: everyone shares the profile XUID here, so the card is
      // the opponent's when a match is on, else this player's.
      std::lock_guard<std::mutex> lock(mutex_);
      if (have_match_ && !match_.opponent.id.empty()) {
        player = match_.opponent.id;
      } else {
        player = id_;
      }
    }
    *id = player;
    std::lock_guard<std::mutex> lock(mutex_);
    *card = PlayerStats{};
    card->id = player;
    if (player == id_ && have_me_) {
      *card = me_;
    } else if (have_match_ && player == match_.opponent.id) {
      *card = match_.opponent;
    } else if (auto it = board_cards_.find(player); it != board_cards_.end()) {
      *card = it->second;
    }
    return !player.empty();
  }

  bool LastBattleResult(BattleResult* out, uint64_t* serial) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (serial) {
      *serial = result_serial_;
    }
    if (!result_serial_) {
      return false;
    }
    *out = last_result_;
    return true;
  }

  // --- HTTP ---
  LeaderboardPage FetchLeaderboard(const std::string& mode, int offset, int limit, bool around_me,
                                   int country, int timeout_ms) {
    LeaderboardPage page;
    std::string base, id;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      base = rest_base_;
      id = id_;
    }
    if (base.empty()) {
      page.error = "online lobby off";
      return page;
    }
    std::string url = fmt::format("{}/leaderboard?mode={}&offset={}&limit={}", base,
                                  mode == "all" ? "all" : "ranked", std::max(0, offset),
                                  std::clamp(limit, 1, 100));
    if (around_me && !id.empty()) {
      url += "&around=" + UrlEncode(id);
    }
    if (country >= 0) {
      url += fmt::format("&country={}", country);
    }
    const HttpResponse res = HttpRequest("GET", url, "", timeout_ms);
    if (res.status != 200) {
      page.error = res.status ? fmt::format("HTTP {}", res.status) : "lobby unreachable";
      REXSYS_WARN("[Ranked] leaderboard {}: {}", url, page.error);
      return page;
    }
    const json j = json::parse(res.body, nullptr, false);
    if (!j.is_object()) {
      page.error = "unreadable answer";
      return page;
    }
    page.ok = true;
    page.mode = JStr(j, "mode");
    page.total = JInt(j, "total");
    page.offset = JInt(j, "offset");
    if (j.contains("rows") && j["rows"].is_array()) {
      std::lock_guard<std::mutex> lock(mutex_);
      for (const json& row : j["rows"]) {
        page.rows.push_back(ParsePlayer(row));
        board_cards_[page.rows.back().id] = page.rows.back();
        XuidForPlayerLocked(page.rows.back().id);
      }
    }
    REXSYS_INFO("[Ranked] leaderboard {} offset {}: {} of {} rows", page.mode, page.offset,
                page.rows.size(), page.total);
    return page;
  }

  bool FetchPlayer(const std::string& id, PlayerStats* out, int timeout_ms) {
    std::string base;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      base = rest_base_;
    }
    if (base.empty() || id.empty()) {
      return false;
    }
    const HttpResponse res = HttpRequest("GET", base + "/players/" + UrlEncode(id), "", timeout_ms);
    if (res.status != 200) {
      return false;
    }
    const json j = json::parse(res.body, nullptr, false);
    if (!j.is_object()) {
      return false;
    }
    *out = ParsePlayer(j);
    return true;
  }

 private:
  uint64_t XuidForPlayerLocked(const std::string& id) {
    if (id.empty()) {
      return 0;
    }
    if (id == id_ && local_xuid_) {
      return local_xuid_;
    }
    // "p" + 16 hex digits: the low 48 bits, tagged so it never equals a profile XUID.
    uint64_t value = 0;
    for (size_t i = 1; i < id.size(); ++i) {
      const char c = id[i];
      value = (value << 4) | uint64_t(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    }
    const uint64_t xuid = 0x0009000000000000ull | (value & 0x0000FFFFFFFFFFFFull);
    xuid_players_[xuid] = id;
    return xuid;
  }

  std::filesystem::path IdentityPathLocked() const {
    return (data_folder_.empty() ? std::filesystem::path(".") : data_folder_) /
           "online_identity.json";
  }

  // The file keeps one identity per lobby (a local test lobby and the real
  // one each have their own players).
  void LoadIdentityLocked() {
    id_.clear();
    key_.clear();
    std::ifstream file(IdentityPathLocked());
    if (!file) {
      return;
    }
    std::stringstream text;
    text << file.rdbuf();
    const json j = json::parse(text.str(), nullptr, false);
    if (!j.is_object() || !j.contains("lobbies") || !j["lobbies"].is_object()) {
      return;
    }
    const json& entry = j["lobbies"].contains(rest_base_) ? j["lobbies"][rest_base_] : json();
    id_ = JStr(entry, "id");
    key_ = JStr(entry, "key");
  }

  void SaveIdentityLocked() {
    const auto path = IdentityPathLocked();
    json j = json::object();
    {
      std::ifstream file(path);
      if (file) {
        std::stringstream text;
        text << file.rdbuf();
        j = json::parse(text.str(), nullptr, false);
        if (!j.is_object()) {
          j = json::object();
        }
      }
    }
    if (!j.contains("lobbies") || !j["lobbies"].is_object()) {
      j["lobbies"] = json::object();
    }
    if (id_.empty()) {
      j["lobbies"].erase(rest_base_);
    } else {
      j["lobbies"][rest_base_] = json{{"id", id_}, {"key", key_}};
    }
    j["note"] = "Your online identity (Ranked Match, leaderboard). Keep this file private: the key "
                "is your password on the online lobby.";
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    // Path operations stay wide: path.string() throws on Windows when the folder has a
    // character the ANSI code page lacks (an Arabic user name on English Windows, say).
    auto temp = path;
    temp += ".tmp";
    {
      std::ofstream file(temp, std::ios::binary | std::ios::trunc);
      file << j.dump(2, ' ', false, json::error_handler_t::replace);
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
      REXSYS_WARN("[Ranked] couldn't save {}: {}", PathText(path), ec.message());
    }
  }

  // A path for logs, as UTF-8 (never throws).
  static std::string PathText(const std::filesystem::path& path) {
    const auto text = path.u8string();  // std::string with -fno-char8_t
    return std::string(text.begin(), text.end());
  }

  void Register() {
    std::string base;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      base = rest_base_;
    }
    const std::string name = PlayerName();
    std::string error;
    for (int attempt = 0; attempt < 3; ++attempt) {
      const HttpResponse res =
          HttpRequest("POST", base + "/register", json{{"name", name}}.dump(), 8000);
      if (res.status == 200) {
        const json j = json::parse(res.body, nullptr, false);
        std::lock_guard<std::mutex> lock(mutex_);
        id_ = JStr(j, "id");
        key_ = JStr(j, "key");
        if (!id_.empty() && !key_.empty()) {
          try {
            SaveIdentityLocked();
          } catch (const std::exception& e) {
            // Keep the identity for this session even if the file can't be written.
            REXSYS_WARN("[Ranked] couldn't save the online identity: {}", e.what());
          }
          REXSYS_WARN("[Ranked] new online identity {} ({}) saved to {}", id_, name,
                      PathText(IdentityPathLocked()));
          register_error_.clear();
          registering_ = false;
          cv_.notify_all();
          return;
        }
        error = "unreadable answer";
        break;
      }
      error = res.status ? fmt::format("HTTP {}", res.status) : "lobby unreachable";
      if (res.status == 429 || res.status == 400) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    REXSYS_WARN("[Ranked] couldn't get an online identity: {}", error);
    std::lock_guard<std::mutex> lock(mutex_);
    register_error_ = error;
    registering_ = false;
    cv_.notify_all();
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::filesystem::path data_folder_;
  std::string rest_base_;
  std::string id_;
  std::string key_;
  bool registering_ = false;
  std::string register_error_;
  bool accepted_ = false;
  bool have_me_ = false;
  PlayerStats me_;
  bool have_match_ = false;
  MatchInfo match_;
  BattleResult last_result_;
  uint64_t result_serial_ = 0;
  bool battle_running_ = false;
  int battle_chars_[2] = {-1, -1};
  uint64_t local_xuid_ = 0;
  std::map<uint64_t, std::string> xuid_players_;
  std::map<std::string, PlayerStats> board_cards_;
  uint64_t card_xuid_ = 0;
  bool card_pending_ = false;
  LeaderboardPage board_page_;
  int64_t board_page_ms_ = 0;
};

thread_local int t_read_target = 0;

}  // namespace

// --- public API ---

void SetDataFolder(const std::filesystem::path& folder) {
  Stats::Get().SetDataFolder(folder);
}

const char* CharacterName(int character) {
  return character >= 0 && character < 21 ? kCharacters[character] : "";
}

bool HasIdentity() {
  return Stats::Get().HasIdentity();
}

std::string IdentityStatus() {
  return Stats::Get().IdentityStatus();
}

bool Me(PlayerStats* out) {
  return Stats::Get().Me(out);
}

LeaderboardPage FetchLeaderboard(const std::string& mode, int offset, int limit, bool around_me,
                                 int country, int timeout_ms) {
  return Stats::Get().FetchLeaderboard(mode, offset, limit, around_me, country, timeout_ms);
}

bool FetchPlayer(const std::string& id, PlayerStats* out, int timeout_ms) {
  return Stats::Get().FetchPlayer(id, out, timeout_ms);
}

bool CurrentMatch(MatchInfo* out) {
  return Stats::Get().CurrentMatch(out);
}

void ReportBattle(const BattleReport& report) {
  Stats::Get().ReportBattle(report);
}

void SetBattleInProgress(bool running, int host_char, int guest_char) {
  Stats::Get().SetBattleInProgress(running, host_char, guest_char);
}

void SetLocalXuid(uint64_t xuid) {
  Stats::Get().SetLocalXuid(xuid);
}

uint64_t LocalXuid() {
  return Stats::Get().LocalXuid();
}

uint64_t XuidForPlayer(const std::string& id) {
  return Stats::Get().XuidForPlayer(id);
}

bool PlayerForXuid(uint64_t xuid, std::string* id) {
  return Stats::Get().PlayerForXuid(xuid, id);
}

void SetReadTarget(int target) {
  t_read_target = target;
}

int ReadTarget() {
  return t_read_target;
}

PlayerStats CardForRead(int target, bool* known) {
  return Stats::Get().CardForRead(target, known);
}

void RequestCard(uint64_t xuid) {
  Stats::Get().RequestCard(xuid);
}

void RememberBoardPage(const LeaderboardPage& page) {
  Stats::Get().RememberBoardPage(page);
}

bool LastBoardPage(LeaderboardPage* out, int64_t* age_ms) {
  return Stats::Get().LastBoardPage(out, age_ms);
}

bool TakeCardRequest(std::string* id, PlayerStats* card) {
  return Stats::Get().TakeCardRequest(id, card);
}

bool LastBattleResult(BattleResult* out, uint64_t* serial) {
  return Stats::Get().LastBattleResult(out, serial);
}

namespace internal {

void OnLobbyMode(const std::string& ws_url) {
  Stats::Get().OnLobbyMode(ws_url);
}

nlohmann::json HelloAuth(int wait_ms) {
  return Stats::Get().HelloAuth(wait_ms);
}

void OnLobbyMessage(const std::string& type, const nlohmann::json& message) {
  Stats::Get().OnLobbyMessage(type, message);
}

void OnLobbyClosed() {
  Stats::Get().OnLobbyClosed();
}

void OnSessionEnded() {
  Stats::Get().OnSessionEnded();
}

}  // namespace internal
}  // namespace rex::net::online::stats

namespace rex::net::online {

std::string LobbyRestBase(const std::string& ws_url) {
  std::string scheme;
  std::string rest;
  if (ws_url.rfind("wss://", 0) == 0) {
    scheme = "https://";
    rest = ws_url.substr(6);
  } else if (ws_url.rfind("ws://", 0) == 0) {
    scheme = "http://";
    rest = ws_url.substr(5);
  } else {
    return {};
  }
  const size_t slash = rest.find('/');
  const std::string host = rest.substr(0, slash);
  if (host.empty()) {
    return {};
  }
  return scheme + host + "/v1";
}

}  // namespace rex::net::online
