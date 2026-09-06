# References

> Verification note (see `README.md` for the full explanation of the
> re-verification gate): entries [1]–[4], [6], [7], [9]–[11], [16], [20],
> [22], [23] were confirmed against ACM DL / arXiv during drafting. Entries
> marked `*(re-verify)*` — [5], [12], [13], [15], [18] — carry 2026 arXiv
> identifiers taken from the project's earlier presentation, did not surface in
> a search, and **must be checked (title, authors, year, venue, ID) or removed
> before submission**. [14], [17], [19] are 2025 works whose final venue should
> be confirmed. [21] is an internal project document.

[1] B. A. Becker, P. Denny, R. Pettit, D. Bouchard, D. J. Bouvier, B.
Harrington, A. Kamil, A. Karkare, C. McDonald, P.-M. Osera, J. L. Pearce, and
J. Prather, "Compiler Error Messages Considered Unhelpful: The Landscape of
Text-Based Programming Error Message Research," in *Proc. Working Group Reports
on Innovation and Technology in Computer Science Education (ITiCSE-WGR '19)*,
2019, pp. 177–210. doi:10.1145/3344429.3372508.

[2] P. Denny, A. Luxton-Reilly, and D. Carpenter, "Enhancing Syntax Error
Messages Appears Ineffectual," in *Proc. 19th ACM Conf. Innovation and
Technology in Computer Science Education (ITiCSE '14)*, Uppsala, Sweden, 2014,
pp. 273–278. doi:10.1145/2591708.2591748.

[3] E. A. Santos and B. A. Becker, "Not the Silver Bullet: LLM-enhanced
Programming Error Messages are Ineffective in Practice," in *Proc. 2024 Conf.
United Kingdom & Ireland Computing Education Research (UKICER '24)*, 2024.
arXiv:2409.18661. doi:10.1145/3689535.3689554.

[4] L. Chen, M. Zaharia, and J. Zou, "FrugalGPT: How to Use Large Language
Models While Reducing Cost and Improving Performance," arXiv:2305.05176, 2023.

[5] D. Nogueira et al., "Unreliable in Practice? A Comprehensive Study of
Errors in LLM-Generated Code," arXiv:2608.00661, 2026. *(re-verify)*

[6] S. Rabanser, N. Rauschmayr, A. Wittmann, P. Kairouz, and others,
"Gatekeeper: Improving Model Cascades Through Confidence Tuning,"
arXiv:2502.19335, 2025. (NeurIPS 2025 poster; ICML 2025.)

[7] X. Chen, M. Lin, N. Schärli, and D. Zhou, "Teaching Large Language Models
to Self-Debug," arXiv:2304.05128, 2023. (ICLR 2024.)

[8] Turflang project, "The Compiler Pipeline: Lexer to Codegen" and "CLI,
Diagnostics & Static Analysis," internal architecture documentation
(`docs/architecture/compiler_pipeline.md`, `docs/reference/cli_and_diagnostics.md`),
2026.

[9] E. A. Santos, P. Prasad, and B. A. Becker, "Always Provide Context: The
Effects of Code Context on Programming Error Message Enhancement," in *Proc.
ACM Conf. Global Computing Education Vol. 1 (CompEd '23)*, Hyderabad, India,
2023, pp. 147–153. doi:10.1145/3576882.3617909.

[10] A. Salmon, R. Hammer, E. A. Santos, and B. A. Becker, "Debugging Without
Error Messages: How LLM Prompting Strategy Affects Programming Error Message
Effectiveness," arXiv:2501.05706, 2025.

[11] Q. Chen, J. Li, C. Deng, J. Yu, M. Chen, and I. Ahmed, "A Deep Dive into
LLM Code Generation Mistakes: What and Why?," arXiv:2411.01414, 2024.

[12] "The New Compiler Stack: A Survey on the Synergy of LLMs and Compilers,"
arXiv:2601.02045, 2026. *(re-verify)*

[13] "An Agentic Harness for Real-World Compilers (llvm-autofix),"
arXiv:2603.20075, 2026. *(re-verify)*

[14] "C3PO: Optimized LLM Cascades with Probabilistic Cost Constraints for
Reasoning," arXiv:2511.07396, 2025. (NeurIPS 2025.)

[15] S. Kotte, "UCCI: Calibrated Uncertainty for Cost-Optimal LLM Cascade
Routing," arXiv:2605.18796, 2026. *(re-verify)*

[16] M. Yue, J. Zhao, M. Zhang, L. Du, and Z. Yao, "Large Language Model
Cascades with Mixture-of-Thoughts Representations for Cost-Efficient
Reasoning," arXiv:2310.03094, 2023. (ICLR 2024.)

[17] C. Fanconi and M. van der Schaar, "Cascaded Language Models for
Cost-Effective Human–AI Decision-Making," arXiv:2506.11887, 2025. (NeurIPS
2025.)

[18] Y. Dong, P. Sharma, B. O'Toole, J. P. Champati, and D. Wu, "Pay for
Hints, Not Answers: LLM Shepherding for Cost-Efficient Inference,"
arXiv:2601.22132, 2026. *(re-verify)*

[19] Google Research, "Speculative Cascades: A Hybrid Approach for Smarter,
Faster LLM Inference," Google Research Blog, Sept. 2025.

[20] L. Chen and G. Varoquaux, "What is the Role of Small Models in the LLM
Era: A Survey," arXiv:2409.06857, 2024.

[21] Turflang project, "Turf-Lang Compiler — Benchmark Report: Supporting Data
for Faculty Benchmark Review," Aug. 2026.

[22] Qwen Team, "Qwen2.5-Coder Technical Report," arXiv:2409.12186, 2024.
(Model used: `qwen2.5-coder-1.5b-instruct`, 4-bit `Q4_K_M` GGUF.)

[23] G. Gerganov and contributors, "llama.cpp: LLM inference in C/C++,"
open-source project, https://github.com/ggml-org/llama.cpp. (Vendored as the
in-process inference backend; GBNF grammar-constrained sampling.)
