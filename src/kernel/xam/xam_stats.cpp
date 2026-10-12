/**
 * @file        kernel/xam/xam_stats.cpp
 * @brief       Xbox LIVE stats (XUserReadStats, stats enumerators / the
 *              leaderboard, gamer cards) answered from the online lobby's
 *              ranked data (rex/net/online_stats.h).
 *
 * Layouts (big-endian guest memory, as the games parse them):
 *   XUSER_STATS_SPEC         136 bytes: +0 view id, +4 column count, +8 u16 column ids[64]
 *   XUSER_STATS_READ_RESULTS   8 bytes: +0 view count, +4 views
 *   XUSER_STATS_VIEW          16 bytes: +0 view id, +4 total rows, +8 row count, +12 rows
 *   XUSER_STATS_ROW           48 bytes: +0 XUID, +8 rank, +16 i64 rating, +24 gamertag[16],
 *                                       +40 column count, +44 columns
 *   XUSER_STATS_COLUMN        24 bytes: +0 u16 column id, +8 XUSER_DATA (+8 type, +16 value)
 *
 * The views a title knows come from its SPA. Burst Limit (424107DC, see
 * C:/rex/_online_tests/ranked/RANKED_RE.md): 145 = Ranked Power Level (world),
 * 146..153 = by country; columns 1 Country (location 0..7), 2 Grade (0 F ..
 * 7 Z); skill views 0xFFFF000x with MU (63) / SIGMA (64). Other views get
 * empty rows.
 */

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/net/online.h>
#include <rex/net/online_stats.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
namespace stats = rex::net::online::stats;

namespace {

constexpr uint32_t kSpecSize = 136;
constexpr uint32_t kViewSize = 16;
constexpr uint32_t kRowSize = 48;
constexpr uint32_t kColumnSize = 24;
constexpr uint8_t kDataInt32 = 1;
constexpr uint8_t kDataInt64 = 2;
constexpr uint8_t kDataDouble = 3;

struct Column {
  uint16_t id = 0;
  uint8_t type = kDataInt32;
  int64_t value = 0;
  double real = 0.0;
};

struct Row {
  uint64_t xuid = 0;
  uint32_t rank = 0;
  int64_t rating = 0;
  std::string gamertag;
  std::vector<Column> columns;
};

struct View {
  uint32_t id = 0;
  uint32_t total_rows = 0;
  std::vector<Row> rows;
};

struct Spec {
  uint32_t view = 0;
  std::vector<uint16_t> columns;
};

std::vector<Spec> ReadSpecs(memory::Memory* memory, uint32_t specs_ptr, uint32_t count) {
  std::vector<Spec> specs;
  if (!specs_ptr) {
    return specs;
  }
  const uint8_t* base = memory->TranslateVirtual(specs_ptr);
  for (uint32_t i = 0; i < count && i < 64; ++i) {
    const uint8_t* spec = base + i * kSpecSize;
    Spec s;
    s.view = memory::load_and_swap<uint32_t>(spec + 0);
    const uint32_t columns = std::min<uint32_t>(memory::load_and_swap<uint32_t>(spec + 4), 64);
    for (uint32_t c = 0; c < columns; ++c) {
      s.columns.push_back(memory::load_and_swap<uint16_t>(spec + 8 + c * 2));
    }
    specs.push_back(std::move(s));
  }
  return specs;
}

/// The country (the game's location 0..7) a power view lists, -1 = world, -2 = not a power view.
int PowerViewCountry(uint32_t view) {
  switch (view) {
    case 145: return -1;
    case 146: return 0;  // Japan
    case 147: return 1;  // U.S.A
    case 148: return 2;  // United Kingdom
    case 149: return 3;  // France
    case 151: return 4;  // Italy
    case 150: return 5;  // Germany
    case 152: return 6;  // Spain
    case 153: return 7;  // Other
    default: return -2;
  }
}

bool IsSkillView(uint32_t view) {
  return (view & 0xFFFF0000u) == 0xFFFF0000u;
}

/// A row of `view` for a player card (`known` false: an unknown player).
Row MakeRow(uint32_t view, const Spec& spec, uint64_t xuid, const stats::PlayerStats& card,
            bool known) {
  Row row;
  row.xuid = xuid;
  row.gamertag = card.name;
  const bool power_view = PowerViewCountry(view) != -2;
  if (known && power_view) {
    // The game takes a row's rating only when it has a rank (else it assumes 5000).
    row.rank = uint32_t(card.rank > 0 ? card.rank : (card.power != 5000 || card.ranked_games ? 1 : 0));
    row.rating = card.power;
  }
  for (uint16_t id : spec.columns) {
    Column column;
    column.id = id;
    if (IsSkillView(view) && (id == 63 || id == 64)) {
      column.type = kDataDouble;  // MU / SIGMA: the TrueSkill defaults
      column.real = id == 63 ? 3.0 : 1.0;
    } else if (power_view && known && id == 1) {
      column.value = std::clamp(card.country, 0, 7);
    } else if (power_view && known && id == 2) {
      column.value = card.grade_index;
    }
    row.columns.push_back(column);
  }
  return row;
}

uint32_t ResultsSize(const std::vector<View>& views) {
  uint32_t size = 8 + uint32_t(views.size()) * kViewSize;
  for (const View& view : views) {
    for (const Row& row : view.rows) {
      size += kRowSize + uint32_t(row.columns.size()) * kColumnSize;
    }
  }
  return size;
}

/// Writes XUSER_STATS_READ_RESULTS at `results_ptr`. False if it doesn't fit.
bool WriteResults(memory::Memory* memory, uint32_t results_ptr, uint32_t size,
                  const std::vector<View>& views) {
  if (!results_ptr || ResultsSize(views) > size) {
    return false;
  }
  uint8_t* base = memory->TranslateVirtual(results_ptr);
  std::memset(base, 0, ResultsSize(views));
  const uint32_t views_ptr = results_ptr + 8;
  memory::store_and_swap<uint32_t>(base + 0, uint32_t(views.size()));
  memory::store_and_swap<uint32_t>(base + 4, views.empty() ? 0 : views_ptr);
  uint32_t next = views_ptr + uint32_t(views.size()) * kViewSize;
  for (size_t v = 0; v < views.size(); ++v) {
    const View& view = views[v];
    uint8_t* out = base + 8 + v * kViewSize;
    const uint32_t rows_ptr = next;
    next += uint32_t(view.rows.size()) * kRowSize;
    memory::store_and_swap<uint32_t>(out + 0, view.id);
    memory::store_and_swap<uint32_t>(out + 4, view.total_rows);
    memory::store_and_swap<uint32_t>(out + 8, uint32_t(view.rows.size()));
    memory::store_and_swap<uint32_t>(out + 12, view.rows.empty() ? 0 : rows_ptr);
    for (size_t r = 0; r < view.rows.size(); ++r) {
      const Row& row = view.rows[r];
      uint8_t* row_out = base + (rows_ptr - results_ptr) + r * kRowSize;
      const uint32_t columns_ptr = next;
      next += uint32_t(row.columns.size()) * kColumnSize;
      memory::store_and_swap<uint64_t>(row_out + 0, row.xuid);
      memory::store_and_swap<uint32_t>(row_out + 8, row.rank);
      memory::store_and_swap<uint64_t>(row_out + 16, uint64_t(row.rating));
      std::strncpy(reinterpret_cast<char*>(row_out + 24), row.gamertag.c_str(), 15);
      memory::store_and_swap<uint32_t>(row_out + 40, uint32_t(row.columns.size()));
      memory::store_and_swap<uint32_t>(row_out + 44, row.columns.empty() ? 0 : columns_ptr);
      for (size_t c = 0; c < row.columns.size(); ++c) {
        const Column& column = row.columns[c];
        uint8_t* col_out = base + (columns_ptr - results_ptr) + c * kColumnSize;
        memory::store_and_swap<uint16_t>(col_out + 0, column.id);
        col_out[8] = column.type;
        if (column.type == kDataDouble) {
          memory::store_and_swap<double>(col_out + 16, column.real);
        } else if (column.type == kDataInt64) {
          memory::store_and_swap<uint64_t>(col_out + 16, uint64_t(column.value));
        } else {
          memory::store_and_swap<uint32_t>(col_out + 16, uint32_t(column.value));
        }
      }
    }
  }
  return true;
}

uint64_t ProfileXuid() {
  auto* profile = REX_KERNEL_STATE()->user_profile();
  const uint64_t xuid = profile ? profile->xuid() : 0;
  if (xuid) {
    stats::SetLocalXuid(xuid);
  }
  return xuid;
}

// The leaderboard reader: one enumerate fills the whole XUSER_STATS_READ_RESULTS
// (the lobby is asked then, off the game thread when the game enumerates with
// an overlapped).
class XStatsEnumerator : public XEnumerator {
 public:
  XStatsEnumerator(KernelState* kernel_state, uint32_t size, uint32_t type, uint64_t pivot,
                   uint32_t rows, std::vector<Spec> specs)
      : XEnumerator(kernel_state, 1, size),
        size_(size),
        type_(type),
        pivot_(pivot),
        rows_(rows),
        specs_(std::move(specs)) {}

  uint32_t WriteItems(uint32_t buffer_ptr, uint8_t* buffer_data, uint32_t* written_count) override {
    if (done_) {
      return X_ERROR_NO_MORE_FILES;
    }
    done_ = true;
    std::vector<View> views;
    uint32_t row_count = 0;
    for (const Spec& spec : specs_) {
      View view;
      view.id = spec.view;
      const int country = PowerViewCountry(spec.view);
      if (country != -2) {
        const bool around_me = type_ == 0;  // by XUID: centred on this player
        const int offset = type_ == 1 ? int(std::max<uint64_t>(pivot_, 1) - 1) : 0;
        const stats::LeaderboardPage page = stats::FetchLeaderboard(
            "ranked", offset, int(rows_), around_me, country >= 0 ? country : -1, 4000);
        if (!page.ok) {
          REXKRNL_WARN("[RankedLive] leaderboard view {} unavailable: {}", spec.view, page.error);
          return X_ERROR_FUNCTION_FAILED;
        }
        view.total_rows = uint32_t(page.total);
        stats::RememberBoardPage(page);
        for (const stats::PlayerStats& card : page.rows) {
          if (view.rows.size() >= rows_) {
            break;
          }
          view.rows.push_back(MakeRow(spec.view, spec, stats::XuidForPlayer(card.id), card, true));
        }
      }
      row_count += uint32_t(view.rows.size());
      views.push_back(std::move(view));
    }
    if (!WriteResults(memory(), buffer_ptr, size_, views)) {
      REXKRNL_WARN("[RankedLive] leaderboard results don't fit {} bytes", size_);
      return X_ERROR_INSUFFICIENT_BUFFER;
    }
    (void)buffer_data;
    REXKRNL_INFO("[RankedLive] leaderboard view {} ({} {}): {} rows",
                 specs_.empty() ? 0 : specs_[0].view, type_ == 1 ? "from rank" : "around XUID",
                 type_ == 1 ? pivot_ : 0, row_count);
    if (written_count) {
      *written_count = row_count;
    }
    return X_ERROR_SUCCESS;
  }

 private:
  uint32_t size_;
  uint32_t type_;
  uint64_t pivot_;
  uint32_t rows_;
  std::vector<Spec> specs_;
  bool done_ = false;
};

}  // namespace

X_HRESULT XamReadStatsFromLobby(memory::Memory* memory, uint32_t xuid_count, uint32_t xuids_ptr,
                                uint32_t spec_count, uint32_t specs_ptr, uint32_t results_size,
                                uint32_t results_ptr) {
  const uint64_t local = ProfileXuid();
  const std::vector<Spec> specs = ReadSpecs(memory, specs_ptr, spec_count);
  // The game reads one player record at a time (burstlimit_ranked.cpp tells
  // which): everyone shares the profile XUID in this recomp.
  const int target = stats::ReadTarget();
  bool known = false;
  const stats::PlayerStats card = stats::CardForRead(target, &known);
  std::vector<View> views;
  for (const Spec& spec : specs) {
    View view;
    view.id = spec.view;
    for (uint32_t i = 0; i < xuid_count && i < 100; ++i) {
      const uint64_t xuid =
          xuids_ptr ? memory::load_and_swap<uint64_t>(memory->TranslateVirtual(xuids_ptr + i * 8))
                    : local;
      std::string id;
      stats::PlayerStats row_card = card;
      bool row_known = known;
      if (xuid != local && stats::PlayerForXuid(xuid, &id)) {
        // A leaderboard player (friends list): no card cached here beyond the name.
        row_known = false;
        row_card = stats::PlayerStats{};
      }
      view.rows.push_back(MakeRow(spec.view, spec, xuid, row_card, row_known));
    }
    view.total_rows = uint32_t(view.rows.size());
    views.push_back(std::move(view));
  }
  if (!WriteResults(memory, results_ptr, results_size, views)) {
    REXKRNL_WARN("[RankedLive] XUserReadStats results don't fit {} bytes (need {})", results_size,
                 ResultsSize(views));
    return X_E_FAIL;
  }
  REXKRNL_INFO("[RankedLive] XUserReadStats {} view(s) x {} xuid(s), {} ({}): power {} grade {} "
               "rank {}",
               specs.size(), xuid_count, target == 1 ? "opponent" : "self",
               known ? card.name : std::string("unknown"), card.power, card.grade, card.rank);
  return X_E_SUCCESS;
}

u32 XamUserCreateStatsEnumerator_entry(u32 title_id, u32 type, u64 pivot, u32 num_rows,
                                       u32 num_specs, u32 specs_ptr, mapped_u32 buffer_size_ptr,
                                       mapped_u32 handle_ptr) {
  if (!num_rows || !num_specs || !specs_ptr || !buffer_size_ptr || !handle_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }
  ProfileXuid();
  auto* memory = REX_KERNEL_MEMORY();
  std::vector<Spec> specs = ReadSpecs(memory, specs_ptr, num_specs);
  num_rows = std::min<u32>(num_rows, 100);
  uint32_t size = 8;
  for (const Spec& spec : specs) {
    size += kViewSize + num_rows * (kRowSize + uint32_t(spec.columns.size()) * kColumnSize);
  }
  *buffer_size_ptr = size;
  auto e = object_ref<XStatsEnumerator>(
      new XStatsEnumerator(REX_KERNEL_STATE(), size, type, pivot, num_rows, std::move(specs)));
  auto result = e->Initialize(0, 0xFB, 0xB0022, 0xB0023, 0);
  if (XFAILED(result)) {
    return result;
  }
  *handle_ptr = e->handle();
  REXKRNL_INFO("[RankedLive] stats enumerator title {:08X} type {} pivot {} rows {} -> {} bytes",
               uint32_t(title_id), uint32_t(type), uint64_t(pivot), uint32_t(num_rows), size);
  return X_ERROR_SUCCESS;
}

u32 XamShowGamerCardUIForXUID_entry(u32 user_index, u64 xuid) {
  REXKRNL_INFO("[RankedLive] gamer card for {:016X}", uint64_t(xuid));
  stats::RequestCard(xuid);
  return X_ERROR_SUCCESS;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamUserCreateStatsEnumerator, rex::kernel::xam::XamUserCreateStatsEnumerator_entry)
REX_EXPORT(__imp__XamShowGamerCardUIForXUID, rex::kernel::xam::XamShowGamerCardUIForXUID_entry)
