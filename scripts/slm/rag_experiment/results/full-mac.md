# RAG experiment: full-mac

Platform: macOS-26.5.2-arm64-arm-64bit-Mach-O  |  model: qwen2.5-coder-1.5b-instruct-q4_k_m.gguf (sha256 cc324af070c2)  |  llama-server: version: 0.7.0 (build 11669, commit 620d2225c)

Cases with every arm complete: 72 (of 72 started). Wilson 95% intervals in brackets.

## Per arm

| arm | n | parsed | abstained | right_line | compiles | correct | exact_text | exact | mean latency s | mean prompt tokens | mean context tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|
| A0_none | 72 | 100.0% [94.9, 100.0] | 0.0% [0.0, 5.1] | 68.1% [56.6, 77.7] | 13.9% [7.7, 23.7] | 13.9% [7.7, 23.7] | 11.1% [5.7, 20.4] | 9.7% [4.8, 18.7] | 2.0 | 1094 | 0 |
| A1_category | 72 | 100.0% [94.9, 100.0] | 0.0% [0.0, 5.1] | 59.7% [48.2, 70.3] | 16.7% [9.8, 26.9] | 15.3% [8.8, 25.3] | 12.5% [6.7, 22.1] | 11.1% [5.7, 20.4] | 3.4 | 1946 | 855 |
| A1p_production | 72 | 100.0% [94.9, 100.0] | 0.0% [0.0, 5.1] | 62.5% [51.0, 72.8] | 20.8% [13.1, 31.6] | 20.8% [13.1, 31.6] | 16.7% [9.8, 26.9] | 12.5% [6.7, 22.1] | 3.9 | 2284 | 1193 |
| A2_bm25_docs | 72 | 100.0% [94.9, 100.0] | 0.0% [0.0, 5.1] | 51.4% [40.1, 62.6] | 18.1% [10.9, 28.5] | 18.1% [10.9, 28.5] | 20.8% [13.1, 31.6] | 16.7% [9.8, 26.9] | 3.8 | 2245 | 1154 |
| A3_examples | 72 | 100.0% [94.9, 100.0] | 0.0% [0.0, 5.1] | 65.3% [53.8, 75.2] | 25.0% [16.4, 36.1] | 25.0% [16.4, 36.1] | 27.8% [18.8, 39.0] | 20.8% [13.1, 31.6] | 2.1 | 1237 | 146 |
| A4_examples_docs | 72 | 100.0% [94.9, 100.0] | 0.0% [0.0, 5.1] | 51.4% [40.1, 62.6] | 18.1% [10.9, 28.5] | 18.1% [10.9, 28.5] | 27.8% [18.8, 39.0] | 16.7% [9.8, 26.9] | 3.9 | 2242 | 1151 |
| A5_random_examples | 72 | 100.0% [94.9, 100.0] | 0.0% [0.0, 5.1] | 58.3% [46.8, 69.0] | 9.7% [4.8, 18.7] | 9.7% [4.8, 18.7] | 15.3% [8.8, 25.3] | 9.7% [4.8, 18.7] | 2.2 | 1238 | 147 |
| A6_production_examples | 72 | 100.0% [94.9, 100.0] | 0.0% [0.0, 5.1] | 58.3% [46.8, 69.0] | 23.6% [15.3, 34.6] | 22.2% [14.2, 33.1] | 27.8% [18.8, 39.0] | 18.1% [10.9, 28.5] | 3.9 | 2280 | 1189 |

## Paired comparison of `correct` (exact McNemar)

b = arm correct and baseline wrong, c = baseline correct and arm wrong.

| arm | vs | arm correct | base correct | b | c | p |
|---|---|---|---|---|---|---|
| A0_none | A1p_production | 10 | 15 | 2 | 7 | 0.1797 |
| A1_category | A1p_production | 11 | 15 | 2 | 6 | 0.2891 |
| A2_bm25_docs | A1p_production | 13 | 15 | 5 | 7 | 0.7744 |
| A3_examples | A1p_production | 18 | 15 | 9 | 6 | 0.6072 |
| A4_examples_docs | A1p_production | 13 | 15 | 4 | 6 | 0.7539 |
| A5_random_examples | A1p_production | 7 | 15 | 2 | 10 | 0.0386 |
| A6_production_examples | A1p_production | 16 | 15 | 5 | 4 | 1.0000 |
| A0_none | A1_category | 10 | 11 | 2 | 3 | 1.0000 |
| A1p_production | A1_category | 15 | 11 | 6 | 2 | 0.2891 |
| A2_bm25_docs | A1_category | 13 | 11 | 4 | 2 | 0.6875 |
| A3_examples | A1_category | 18 | 11 | 10 | 3 | 0.0923 |
| A4_examples_docs | A1_category | 13 | 11 | 5 | 3 | 0.7266 |
| A5_random_examples | A1_category | 7 | 11 | 1 | 5 | 0.2188 |
| A6_production_examples | A1_category | 16 | 11 | 6 | 1 | 0.1250 |
| A1_category | A0_none | 11 | 10 | 3 | 2 | 1.0000 |
| A1p_production | A0_none | 15 | 10 | 7 | 2 | 0.1797 |
| A2_bm25_docs | A0_none | 13 | 10 | 7 | 4 | 0.5488 |
| A3_examples | A0_none | 18 | 10 | 10 | 2 | 0.0386 |
| A4_examples_docs | A0_none | 13 | 10 | 7 | 4 | 0.5488 |
| A5_random_examples | A0_none | 7 | 10 | 2 | 5 | 0.4531 |
| A6_production_examples | A0_none | 16 | 10 | 8 | 2 | 0.1094 |

## `correct` by subset

| subset | n | A0_none | A1_category | A1p_production | A2_bm25_docs | A3_examples | A4_examples_docs | A5_random_examples | A6_production_examples |
|---|---|---|---|---|---|---|---|---|---|
| foreign syntax | 61 | 7/61 | 8/61 | 11/61 | 9/61 | 16/61 | 10/61 | 5/61 | 12/61 |
| classic | 11 | 3/11 | 3/11 | 4/11 | 4/11 | 2/11 | 3/11 | 2/11 | 4/11 |
| pair seen in train | 14 | 0/14 | 1/14 | 0/14 | 1/14 | 3/14 | 1/14 | 0/14 | 1/14 |
| novel pair | 58 | 10/58 | 10/58 | 15/58 | 12/58 | 15/58 | 12/58 | 7/58 | 15/58 |

## `correct` by kind

| kind | n | A0_none | A1_category | A1p_production | A2_bm25_docs | A3_examples | A4_examples_docs | A5_random_examples | A6_production_examples |
|---|---|---|---|---|---|---|---|---|---|
| delete_semicolon | 1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 |
| drop_closing_brace | 2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 |
| drop_paren | 2 | 1/2 | 1/2 | 2/2 | 2/2 | 1/2 | 2/2 | 1/2 | 2/2 |
| foreign_array_brace (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 | 0/2 | 1/2 | 0/2 | 0/2 | 0/2 |
| foreign_arrow_call (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 |
| foreign_bool_literal (foreign syntax) | 2 | 2/2 | 2/2 | 1/2 | 1/2 | 1/2 | 1/2 | 1/2 | 1/2 |
| foreign_break (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 |
| foreign_case (foreign syntax) | 1 | 0/1 | 0/1 | 1/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 |
| foreign_cast_c (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 |
| foreign_cast_call (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 1/2 |
| foreign_comment_hash (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 | 1/2 | 0/2 | 0/2 | 0/2 | 0/2 |
| foreign_const_keyword (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 |
| foreign_ctor_braces (foreign syntax) | 2 | 0/2 | 1/2 | 0/2 | 1/2 | 2/2 | 1/2 | 0/2 | 2/2 |
| foreign_ctor_call (foreign syntax) | 2 | 1/2 | 1/2 | 2/2 | 1/2 | 1/2 | 2/2 | 1/2 | 1/2 |
| foreign_decl_auto (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 |
| foreign_decl_c (foreign syntax) | 2 | 0/2 | 1/2 | 1/2 | 2/2 | 1/2 | 2/2 | 1/2 | 1/2 |
| foreign_decl_const (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 |
| foreign_decl_let (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 |
| foreign_decl_let_mut (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 |
| foreign_decl_var (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 |
| foreign_decl_walrus (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 |
| foreign_default (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 |
| foreign_field_c (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 |
| foreign_fn_keyword (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 |
| foreign_for_c (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 |
| foreign_for_paren (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 |
| foreign_for_range (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 |
| foreign_import (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 | 0/2 | 2/2 | 0/2 | 0/2 | 0/2 |
| foreign_length (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 |
| foreign_logic_word (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 |
| foreign_method_decl (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 |
| foreign_module_dot (foreign syntax) | 2 | 2/2 | 0/2 | 1/2 | 0/2 | 2/2 | 1/2 | 0/2 | 1/2 |
| foreign_new_keyword (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 1/1 | 1/1 | 1/1 | 0/1 | 1/1 |
| foreign_null (foreign syntax) | 2 | 2/2 | 2/2 | 2/2 | 1/2 | 2/2 | 1/2 | 1/2 | 2/2 |
| foreign_param_c (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 |
| foreign_print (foreign syntax) | 2 | 0/2 | 0/2 | 2/2 | 0/2 | 2/2 | 0/2 | 0/2 | 1/2 |
| foreign_return_arrow (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 |
| foreign_static_dot (foreign syntax) | 2 | 0/2 | 1/2 | 1/2 | 1/2 | 1/2 | 1/2 | 1/2 | 1/2 |
| foreign_switch (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 |
| foreign_ternary (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 |
| foreign_tostring (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 |
| swap_return_type | 2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 |
| swap_type | 2 | 1/2 | 1/2 | 1/2 | 1/2 | 0/2 | 0/2 | 0/2 | 1/2 |
| typo_identifier | 2 | 1/2 | 1/2 | 1/2 | 1/2 | 1/2 | 1/2 | 1/2 | 1/2 |
