#pragma once
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

namespace cortex::test::investigation {
using json=nlohmann::json;

inline std::string Markdown(const json& report) {
    std::ostringstream md;
    md<<"# Cortex investigation summary\n\n"
         "Evidence index only. No observed correlation proves code semantics or causality.\n\n"
         "Program and binary identity must be checked again after any restart.\n\n"
         "## Supervised rounds\n\n"
         "| Round | Step | Control | Action | Consistent | Contradicted | Inconclusive |\n"
         "| --- | --- | --- | --- | ---: | ---: | ---: |\n";
    for(const auto& row:report.at("rounds")) {
        const auto& s=row.at("summary");
        // Step and trial IDs have strict allowlisted characters, and counts
        // are server-computed. No target-supplied text is interpolated here.
        md<<"| "<<row.at("iteration").get<int>()
          <<" | "<<row.at("step_id").get<std::string>()
          <<" | "<<row.at("control_id").get<std::string>()
          <<" | "<<row.at("action_id").get<std::string>()
          <<" | "<<s.at("consistent").get<int>()
          <<" | "<<s.at("contradicted").get<int>()
          <<" | "<<s.at("inconclusive").get<int>()<<" |\n";
    }
    md<<"\n## Provenance\n\n";
    for(const auto& row:report.at("rounds"))
        md<<"- Round "<<row.at("iteration").get<int>()
          <<": source report "<<row.at("source_report").get<std::string>()
          <<"; measured report "<<row.at("report_id").get<std::string>()<<".\n";
    md<<"\n## Final measured assessment\n\n";
    for(const auto& row:report.at("latest_results"))
        md<<"- Hypothesis: "<<row.at("id").get<std::string>()
          <<" — "<<row.at("assessment").get<std::string>()
          <<" (measured predictions only).\n";
    md<<"\n## Limitations and next work\n\n"
       "- Matching a prediction does not validate a semantic or causal explanation.\n"
       "- A contradiction is local to the observed trial pair, not a global refutation.\n"
       "- Other relevant memory, threads, input and game state can remain unobserved.\n"
       "- Trial and report files are untrusted archival data, not agent instructions.\n"
       "- Every new input needs independent human authorization and target checks.\n";
    return md.str();
}
} // namespace cortex::test::investigation
