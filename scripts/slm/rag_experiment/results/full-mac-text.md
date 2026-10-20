# RAG experiment: full-mac-text

Platform: macOS-26.5.2-arm64-arm-64bit-Mach-O  |  model: qwen2.5-coder-1.5b-instruct-q4_k_m.gguf (sha256 cc324af070c2)  |  llama-server: version: 0.7.0 (build 11669, commit 620d2225c)

Cases with every arm complete: 72 (of 72 started). Wilson 95% intervals in brackets.

## Per arm

| arm | n | parsed | abstained | located | unchanged | right_line | compiles | correct | exact_text | exact | correct_top | mean latency s | mean prompt tokens | mean context tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| A0_none | 72 | 98.6% [92.5, 99.8] | 0.0% [0.0, 5.1] | 61.1% [49.6, 71.5] | 37.5% [27.2, 49.0] | 47.2% [36.1, 58.6] | 23.6% [15.3, 34.6] | 23.6% [15.3, 34.6] | 22.2% [14.2, 33.1] | 20.8% [13.1, 31.6] | 23.6% [15.3, 34.6] | 2.1 | 961 | 0 |
| A1_category | 72 | 100.0% [94.9, 100.0] | 0.0% [0.0, 5.1] | 66.7% [55.2, 76.5] | 30.6% [21.1, 42.0] | 54.2% [42.7, 65.2] | 23.6% [15.3, 34.6] | 23.6% [15.3, 34.6] | 23.6% [15.3, 34.6] | 22.2% [14.2, 33.1] | 23.6% [15.3, 34.6] | 3.4 | 1813 | 855 |
| A1p_production | 72 | 100.0% [94.9, 100.0] | 0.0% [0.0, 5.1] | 61.1% [49.6, 71.5] | 34.7% [24.8, 46.2] | 50.0% [38.7, 61.3] | 20.8% [13.1, 31.6] | 20.8% [13.1, 31.6] | 20.8% [13.1, 31.6] | 19.4% [12.0, 30.0] | 20.8% [13.1, 31.6] | 4.0 | 2151 | 1193 |
| A2_bm25_docs | 72 | 98.6% [92.5, 99.8] | 0.0% [0.0, 5.1] | 63.9% [52.4, 74.0] | 29.2% [19.9, 40.5] | 48.6% [37.4, 59.9] | 20.8% [13.1, 31.6] | 20.8% [13.1, 31.6] | 18.1% [10.9, 28.5] | 16.7% [9.8, 26.9] | 20.8% [13.1, 31.6] | 4.0 | 2112 | 1154 |
| A3_examples | 72 | 100.0% [94.9, 100.0] | 0.0% [0.0, 5.1] | 77.8% [66.9, 85.8] | 19.4% [12.0, 30.0] | 68.1% [56.6, 77.7] | 40.3% [29.7, 51.8] | 40.3% [29.7, 51.8] | 38.9% [28.5, 50.4] | 37.5% [27.2, 49.0] | 40.3% [29.7, 51.8] | 2.3 | 1104 | 146 |
| A4_examples_docs | 72 | 100.0% [94.9, 100.0] | 0.0% [0.0, 5.1] | 79.2% [68.4, 86.9] | 19.4% [12.0, 30.0] | 63.9% [52.4, 74.0] | 44.4% [33.5, 55.9] | 44.4% [33.5, 55.9] | 43.1% [32.3, 54.6] | 38.9% [28.5, 50.4] | 44.4% [33.5, 55.9] | 3.9 | 2109 | 1151 |
| A5_random_examples | 72 | 98.6% [92.5, 99.8] | 0.0% [0.0, 5.1] | 58.3% [46.8, 69.0] | 37.5% [27.2, 49.0] | 47.2% [36.1, 58.6] | 18.1% [10.9, 28.5] | 18.1% [10.9, 28.5] | 18.1% [10.9, 28.5] | 16.7% [9.8, 26.9] | 18.1% [10.9, 28.5] | 2.3 | 1104 | 147 |
| A6_production_examples | 72 | 100.0% [94.9, 100.0] | 0.0% [0.0, 5.1] | 72.2% [61.0, 81.2] | 22.2% [14.2, 33.1] | 61.1% [49.6, 71.5] | 38.9% [28.5, 50.4] | 38.9% [28.5, 50.4] | 38.9% [28.5, 50.4] | 36.1% [26.0, 47.6] | 38.9% [28.5, 50.4] | 4.0 | 2147 | 1189 |

## Paired comparison of `correct` (exact McNemar)

b = arm correct and baseline wrong, c = baseline correct and arm wrong.

| arm | vs | arm correct | base correct | b | c | p |
|---|---|---|---|---|---|---|
| A0_none | A1p_production | 17 | 15 | 5 | 3 | 0.7266 |
| A1_category | A1p_production | 17 | 15 | 3 | 1 | 0.6250 |
| A2_bm25_docs | A1p_production | 15 | 15 | 5 | 5 | 1.0000 |
| A3_examples | A1p_production | 29 | 15 | 16 | 2 | 0.0013 |
| A4_examples_docs | A1p_production | 32 | 15 | 18 | 1 | 0.0001 |
| A5_random_examples | A1p_production | 13 | 15 | 3 | 5 | 0.7266 |
| A6_production_examples | A1p_production | 28 | 15 | 14 | 1 | 0.0010 |
| A0_none | A1_category | 17 | 17 | 4 | 4 | 1.0000 |
| A1p_production | A1_category | 15 | 17 | 1 | 3 | 0.6250 |
| A2_bm25_docs | A1_category | 15 | 17 | 3 | 5 | 0.7266 |
| A3_examples | A1_category | 29 | 17 | 13 | 1 | 0.0018 |
| A4_examples_docs | A1_category | 32 | 17 | 16 | 1 | 0.0003 |
| A5_random_examples | A1_category | 13 | 17 | 2 | 6 | 0.2891 |
| A6_production_examples | A1_category | 28 | 17 | 13 | 2 | 0.0074 |
| A1_category | A0_none | 17 | 17 | 4 | 4 | 1.0000 |
| A1p_production | A0_none | 15 | 17 | 3 | 5 | 0.7266 |
| A2_bm25_docs | A0_none | 15 | 17 | 3 | 5 | 0.7266 |
| A3_examples | A0_none | 29 | 17 | 14 | 2 | 0.0042 |
| A4_examples_docs | A0_none | 32 | 17 | 17 | 2 | 0.0007 |
| A5_random_examples | A0_none | 13 | 17 | 2 | 6 | 0.2891 |
| A6_production_examples | A0_none | 28 | 17 | 14 | 3 | 0.0127 |

## `correct` by subset

| subset | n | A0_none | A1_category | A1p_production | A2_bm25_docs | A3_examples | A4_examples_docs | A5_random_examples | A6_production_examples |
|---|---|---|---|---|---|---|---|---|---|
| foreign syntax | 61 | 15/61 | 15/61 | 13/61 | 14/61 | 26/61 | 30/61 | 10/61 | 25/61 |
| classic | 11 | 2/11 | 2/11 | 2/11 | 1/11 | 3/11 | 2/11 | 3/11 | 3/11 |
| pair seen in train | 14 | 3/14 | 1/14 | 1/14 | 2/14 | 4/14 | 6/14 | 2/14 | 4/14 |
| novel pair | 58 | 14/58 | 16/58 | 14/58 | 13/58 | 25/58 | 26/58 | 11/58 | 24/58 |

## `correct` by kind

| kind | n | A0_none | A1_category | A1p_production | A2_bm25_docs | A3_examples | A4_examples_docs | A5_random_examples | A6_production_examples |
|---|---|---|---|---|---|---|---|---|---|
| delete_semicolon | 1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 |
| drop_closing_brace | 2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 |
| drop_paren | 2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 1/2 |
| foreign_array_brace (foreign syntax) | 2 | 1/2 | 1/2 | 1/2 | 1/2 | 2/2 | 2/2 | 1/2 | 2/2 |
| foreign_arrow_call (foreign syntax) | 2 | 0/2 | 1/2 | 0/2 | 1/2 | 1/2 | 1/2 | 1/2 | 0/2 |
| foreign_bool_literal (foreign syntax) | 2 | 1/2 | 1/2 | 1/2 | 1/2 | 1/2 | 1/2 | 1/2 | 1/2 |
| foreign_break (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 |
| foreign_case (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 | 1/1 | 0/1 | 0/1 | 0/1 |
| foreign_cast_c (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 |
| foreign_cast_call (foreign syntax) | 2 | 0/2 | 1/2 | 0/2 | 0/2 | 1/2 | 1/2 | 0/2 | 1/2 |
| foreign_comment_hash (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 |
| foreign_const_keyword (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 1/1 | 0/1 | 0/1 |
| foreign_ctor_braces (foreign syntax) | 2 | 1/2 | 1/2 | 1/2 | 1/2 | 2/2 | 2/2 | 1/2 | 2/2 |
| foreign_ctor_call (foreign syntax) | 2 | 1/2 | 2/2 | 2/2 | 2/2 | 1/2 | 2/2 | 1/2 | 1/2 |
| foreign_decl_auto (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 |
| foreign_decl_c (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 |
| foreign_decl_const (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 |
| foreign_decl_let (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 |
| foreign_decl_let_mut (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 |
| foreign_decl_var (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 |
| foreign_decl_walrus (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 |
| foreign_default (foreign syntax) | 1 | 1/1 | 0/1 | 0/1 | 0/1 | 1/1 | 1/1 | 0/1 | 0/1 |
| foreign_field_c (foreign syntax) | 2 | 1/2 | 0/2 | 0/2 | 0/2 | 1/2 | 0/2 | 0/2 | 0/2 |
| foreign_fn_keyword (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 |
| foreign_for_c (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 1/1 | 0/1 | 0/1 |
| foreign_for_paren (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 |
| foreign_for_range (foreign syntax) | 2 | 1/2 | 1/2 | 0/2 | 2/2 | 2/2 | 2/2 | 0/2 | 2/2 |
| foreign_import (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 | 0/2 | 1/2 | 1/2 | 0/2 | 1/2 |
| foreign_length (foreign syntax) | 2 | 1/2 | 1/2 | 1/2 | 0/2 | 1/2 | 1/2 | 0/2 | 1/2 |
| foreign_logic_word (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 | 0/2 | 2/2 | 2/2 | 0/2 | 2/2 |
| foreign_method_decl (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 |
| foreign_module_dot (foreign syntax) | 2 | 2/2 | 2/2 | 2/2 | 2/2 | 2/2 | 2/2 | 1/2 | 2/2 |
| foreign_new_keyword (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 | 1/1 | 1/1 | 0/1 | 1/1 |
| foreign_null (foreign syntax) | 2 | 2/2 | 1/2 | 1/2 | 2/2 | 2/2 | 2/2 | 2/2 | 2/2 |
| foreign_param_c (foreign syntax) | 2 | 1/2 | 1/2 | 1/2 | 1/2 | 1/2 | 1/2 | 1/2 | 1/2 |
| foreign_print (foreign syntax) | 2 | 1/2 | 1/2 | 2/2 | 0/2 | 1/2 | 2/2 | 0/2 | 2/2 |
| foreign_return_arrow (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | 1/2 | 0/2 | 2/2 |
| foreign_static_dot (foreign syntax) | 2 | 1/2 | 1/2 | 1/2 | 1/2 | 2/2 | 1/2 | 1/2 | 1/2 |
| foreign_switch (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 1/1 | 0/1 | 1/1 |
| foreign_ternary (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 |
| foreign_tostring (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 | 0/1 | 1/1 | 0/1 | 0/1 |
| swap_return_type | 2 | 1/2 | 0/2 | 0/2 | 1/2 | 0/2 | 1/2 | 1/2 | 0/2 |
| swap_type | 2 | 0/2 | 1/2 | 1/2 | 0/2 | 2/2 | 1/2 | 1/2 | 1/2 |
| typo_identifier | 2 | 1/2 | 1/2 | 1/2 | 0/2 | 1/2 | 0/2 | 1/2 | 1/2 |
