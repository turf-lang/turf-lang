# RAG experiment: qwen7b-text

Platform: macOS-26.5.2-arm64-arm-64bit-Mach-O  |  model: qwen2.5-coder-7b-instruct-q4_k_m.gguf (sha256 509287f78cb4)  |  llama-server: version: 0.7.0 (build 11669, commit 620d2225c)

Cases with every arm complete: 72 (of 72 started). Wilson 95% intervals in brackets.

## Per arm

| arm | n | parsed | abstained | located | unchanged | right_line | compiles | correct | exact_text | exact | correct_top | mean latency s | mean prompt tokens | mean context tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| A0_none | 72 | 100.0% [94.9, 100.0] | 0.0% [0.0, 5.1] | 87.5% [77.9, 93.3] | 12.5% [6.7, 22.1] | 69.4% [58.0, 78.9] | 41.7% [31.0, 53.2] | 40.3% [29.7, 51.8] | 38.9% [28.5, 50.4] | 38.9% [28.5, 50.4] | 40.3% [29.7, 51.8] | 9.3 | 961 | 0 |
| A1p_production | 72 | 98.6% [92.5, 99.8] | 0.0% [0.0, 5.1] | 81.9% [71.5, 89.1] | 16.7% [9.8, 26.9] | 65.3% [53.8, 75.2] | 52.8% [41.4, 63.9] | 51.4% [40.1, 62.6] | 45.8% [34.8, 57.3] | 45.8% [34.8, 57.3] | 51.4% [40.1, 62.6] | 18.4 | 2151 | 1193 |
| A3_examples | 72 | 98.6% [92.5, 99.8] | 0.0% [0.0, 5.1] | 91.7% [83.0, 96.1] | 6.9% [3.0, 15.2] | 75.0% [63.9, 83.6] | 68.1% [56.6, 77.7] | 66.7% [55.2, 76.5] | 65.3% [53.8, 75.2] | 61.1% [49.6, 71.5] | 66.7% [55.2, 76.5] | 10.6 | 1104 | 146 |

## Paired comparison of `correct` (exact McNemar)

b = arm correct and baseline wrong, c = baseline correct and arm wrong.

| arm | vs | arm correct | base correct | b | c | p |
|---|---|---|---|---|---|---|
| A0_none | A1p_production | 29 | 37 | 4 | 12 | 0.0768 |
| A3_examples | A1p_production | 48 | 37 | 13 | 2 | 0.0074 |
| A1p_production | A0_none | 37 | 29 | 12 | 4 | 0.0768 |
| A3_examples | A0_none | 48 | 29 | 20 | 1 | 0.0000 |

## `correct` by subset

| subset | n | A0_none | A1p_production | A3_examples |
|---|---|---|---|---|
| foreign syntax | 61 | 24/61 | 32/61 | 43/61 |
| classic | 11 | 5/11 | 5/11 | 5/11 |
| pair seen in train | 14 | 1/14 | 5/14 | 5/14 |
| novel pair | 58 | 28/58 | 32/58 | 43/58 |

## `correct` by kind

| kind | n | A0_none | A1p_production | A3_examples |
|---|---|---|---|---|
| delete_semicolon | 1 | 0/1 | 0/1 | 0/1 |
| drop_closing_brace | 2 | 0/2 | 0/2 | 0/2 |
| drop_paren | 2 | 2/2 | 2/2 | 2/2 |
| foreign_array_brace (foreign syntax) | 2 | 2/2 | 2/2 | 2/2 |
| foreign_arrow_call (foreign syntax) | 2 | 1/2 | 0/2 | 1/2 |
| foreign_bool_literal (foreign syntax) | 2 | 2/2 | 2/2 | 2/2 |
| foreign_break (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 |
| foreign_case (foreign syntax) | 1 | 1/1 | 1/1 | 1/1 |
| foreign_cast_c (foreign syntax) | 1 | 0/1 | 0/1 | 1/1 |
| foreign_cast_call (foreign syntax) | 2 | 1/2 | 1/2 | 2/2 |
| foreign_comment_hash (foreign syntax) | 2 | 1/2 | 1/2 | 1/2 |
| foreign_const_keyword (foreign syntax) | 1 | 0/1 | 0/1 | 1/1 |
| foreign_ctor_braces (foreign syntax) | 2 | 0/2 | 2/2 | 2/2 |
| foreign_ctor_call (foreign syntax) | 2 | 1/2 | 2/2 | 1/2 |
| foreign_decl_auto (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| foreign_decl_c (foreign syntax) | 2 | 1/2 | 2/2 | 2/2 |
| foreign_decl_const (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| foreign_decl_let (foreign syntax) | 1 | 0/1 | 0/1 | 1/1 |
| foreign_decl_let_mut (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| foreign_decl_var (foreign syntax) | 1 | 0/1 | 0/1 | 1/1 |
| foreign_decl_walrus (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| foreign_default (foreign syntax) | 1 | 1/1 | 1/1 | 1/1 |
| foreign_field_c (foreign syntax) | 2 | 1/2 | 2/2 | 1/2 |
| foreign_fn_keyword (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 |
| foreign_for_c (foreign syntax) | 1 | 0/1 | 1/1 | 1/1 |
| foreign_for_paren (foreign syntax) | 2 | 1/2 | 0/2 | 2/2 |
| foreign_for_range (foreign syntax) | 2 | 1/2 | 1/2 | 2/2 |
| foreign_import (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 |
| foreign_length (foreign syntax) | 2 | 2/2 | 2/2 | 2/2 |
| foreign_logic_word (foreign syntax) | 2 | 0/2 | 0/2 | 2/2 |
| foreign_method_decl (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 |
| foreign_module_dot (foreign syntax) | 2 | 2/2 | 2/2 | 2/2 |
| foreign_new_keyword (foreign syntax) | 1 | 0/1 | 0/1 | 1/1 |
| foreign_null (foreign syntax) | 2 | 2/2 | 2/2 | 2/2 |
| foreign_param_c (foreign syntax) | 2 | 1/2 | 1/2 | 2/2 |
| foreign_print (foreign syntax) | 2 | 2/2 | 2/2 | 2/2 |
| foreign_return_arrow (foreign syntax) | 2 | 0/2 | 2/2 | 2/2 |
| foreign_static_dot (foreign syntax) | 2 | 1/2 | 2/2 | 2/2 |
| foreign_switch (foreign syntax) | 1 | 0/1 | 1/1 | 1/1 |
| foreign_ternary (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| foreign_tostring (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| swap_return_type | 2 | 0/2 | 0/2 | 0/2 |
| swap_type | 2 | 1/2 | 1/2 | 1/2 |
| typo_identifier | 2 | 2/2 | 2/2 | 2/2 |
