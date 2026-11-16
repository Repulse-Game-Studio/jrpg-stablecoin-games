// No Python twin: Tier2Deferred.<id> replays each ported Tier 2 vector (tier2::ported) a second time with a transport
// that completes requests only from the runner's pump loop, one per turn, as Unreal's HTTP callbacks complete them on
// a later tick (unity/core-tests/Tier2SingleThreadTest.cs's role): every networked step must suspend, and resume
// correctly from the pump.
#include "support/Test.h"
#include "support/Tier2.h"
#include "support/Vectors.h"

#include "Scfg/Json.h"
#include "Scfg/Result.h"

#include <optional>
#include <string>

using scfg::Json;
using scfg::Result;
namespace tier2 = scfg_test::tier2;

SCFG_TEST_SOURCE(Tier2DeferredVectors) {
    for (const std::string& id : scfg_test::vectors::tier2_ids()) {
        if (!tier2::ported(id)) continue;
        scfg_test::add("Tier2Deferred", id, [id](scfg_test::Ctx& t) {
            std::optional<std::string> text = scfg_test::vectors::read_text("tier2/" + id + ".json");
            if (!t.check(text.has_value(), "tier2/" + id + " cannot be read")) return;
            Result<Json> v = tier2::parse(*text);
            if (!t.check(v.ok(), "tier2/" + id + " cannot be parsed")) return;
            tier2::ReplayTransport transport(tier2::ReplayMode::Deferred);
            std::optional<std::string> diff = tier2::replay(v.value(), transport);
            t.check(!diff.has_value(), "tier2/" + id + " (deferred): " + diff.value_or(""));
            t.check(transport.queued() == 0, "tier2/" + id + ": requests left in the queue");
        });
    }
}
