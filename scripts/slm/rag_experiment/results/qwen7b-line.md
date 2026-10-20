# RAG experiment: qwen7b-line

Platform: macOS-26.5.2-arm64-arm-64bit-Mach-O  |  model: qwen2.5-coder-7b-instruct-q4_k_m.gguf (sha256 509287f78cb4)  |  llama-server: version: 0.7.0 (build 11669, commit 620d2225c)

Cases with every arm complete: 72 (of 72 started). Wilson 95% intervals in brackets.

## Per arm

| arm | n | parsed | abstained | right_line | compiles | correct | exact_text | exact | mean latency s | mean prompt tokens | mean context tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|
| A0_none | 72 | 100.0% [94.9, 100.0] | 2.8% [0.8, 9.6] | 76.4% [65.4, 84.7] | 38.9% [28.5, 50.4] | 37.5% [27.2, 49.0] | 33.3% [23.5, 44.8] | 31.9% [22.3, 43.4] | 9.3 | 1094 | 0 |
| A1p_production | 72 | 100.0% [94.9, 100.0] | 1.4% [0.2, 7.5] | 77.8% [66.9, 85.8] | 37.5% [27.2, 49.0] | 36.1% [26.0, 47.6] | 30.6% [21.1, 42.0] | 29.2% [19.9, 40.5] | 18.1 | 2284 | 1193 |
| A3_examples | 72 | 98.6% [92.5, 99.8] | 1.4% [0.2, 7.5] | 77.8% [66.9, 85.8] | 61.1% [49.6, 71.5] | 59.7% [48.2, 70.3] | 56.9% [45.4, 67.7] | 50.0% [38.7, 61.3] | 10.4 | 1237 | 146 |

## Paired comparison of `correct` (exact McNemar)

b = arm correct and baseline wrong, c = baseline correct and arm wrong.

| arm | vs | arm correct | base correct | b | c | p |
|---|---|---|---|---|---|---|
| A0_none | A1p_production | 27 | 26 | 8 | 7 | 1.0000 |
| A3_examples | A1p_production | 43 | 26 | 19 | 2 | 0.0002 |
| A1p_production | A0_none | 26 | 27 | 7 | 8 | 1.0000 |
| A3_examples | A0_none | 43 | 27 | 18 | 2 | 0.0004 |

## `correct` by subset

| subset | n | A0_none | A1p_production | A3_examples |
|---|---|---|---|---|
| foreign syntax | 61 | 22/61 | 22/61 | 39/61 |
| classic | 11 | 5/11 | 4/11 | 4/11 |
| pair seen in train | 14 | 3/14 | 4/14 | 7/14 |
| novel pair | 58 | 24/58 | 22/58 | 36/58 |

## `correct` by kind

| kind | n | A0_none | A1p_production | A3_examples |
|---|---|---|---|---|
| delete_semicolon | 1 | 0/1 | 0/1 | 0/1 |
| drop_closing_brace | 2 | 0/2 | 0/2 | 0/2 |
| drop_paren | 2 | 1/2 | 1/2 | 1/2 |
| foreign_array_brace (foreign syntax) | 2 | 2/2 | 1/2 | 2/2 |
| foreign_arrow_call (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 |
| foreign_bool_literal (foreign syntax) | 2 | 2/2 | 2/2 | 2/2 |
| foreign_break (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 |
| foreign_case (foreign syntax) | 1 | 1/1 | 1/1 | 1/1 |
| foreign_cast_c (foreign syntax) | 1 | 0/1 | 0/1 | 1/1 |
| foreign_cast_call (foreign syntax) | 2 | 1/2 | 0/2 | 2/2 |
| foreign_comment_hash (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 |
| foreign_const_keyword (foreign syntax) | 1 | 0/1 | 0/1 | 1/1 |
| foreign_ctor_braces (foreign syntax) | 2 | 2/2 | 2/2 | 2/2 |
| foreign_ctor_call (foreign syntax) | 2 | 1/2 | 2/2 | 1/2 |
| foreign_decl_auto (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| foreign_decl_c (foreign syntax) | 2 | 0/2 | 1/2 | 1/2 |
| foreign_decl_const (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| foreign_decl_let (foreign syntax) | 1 | 0/1 | 0/1 | 1/1 |
| foreign_decl_let_mut (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| foreign_decl_var (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| foreign_decl_walrus (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| foreign_default (foreign syntax) | 1 | 1/1 | 0/1 | 1/1 |
| foreign_field_c (foreign syntax) | 2 | 2/2 | 1/2 | 2/2 |
| foreign_fn_keyword (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 |
| foreign_for_c (foreign syntax) | 1 | 0/1 | 1/1 | 1/1 |
| foreign_for_paren (foreign syntax) | 2 | 0/2 | 0/2 | 1/2 |
| foreign_for_range (foreign syntax) | 2 | 0/2 | 1/2 | 2/2 |
| foreign_import (foreign syntax) | 2 | 0/2 | 0/2 | 1/2 |
| foreign_length (foreign syntax) | 2 | 2/2 | 2/2 | 2/2 |
| foreign_logic_word (foreign syntax) | 2 | 0/2 | 0/2 | 2/2 |
| foreign_method_decl (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 |
| foreign_module_dot (foreign syntax) | 2 | 2/2 | 2/2 | 2/2 |
| foreign_new_keyword (foreign syntax) | 1 | 0/1 | 0/1 | 1/1 |
| foreign_null (foreign syntax) | 2 | 1/2 | 1/2 | 2/2 |
| foreign_param_c (foreign syntax) | 2 | 2/2 | 0/2 | 2/2 |
| foreign_print (foreign syntax) | 2 | 2/2 | 2/2 | 2/2 |
| foreign_return_arrow (foreign syntax) | 2 | 0/2 | 2/2 | 2/2 |
| foreign_static_dot (foreign syntax) | 2 | 1/2 | 1/2 | 1/2 |
| foreign_switch (foreign syntax) | 1 | 0/1 | 0/1 | 1/1 |
| foreign_ternary (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| foreign_tostring (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| swap_return_type | 2 | 1/2 | 0/2 | 1/2 |
| swap_type | 2 | 1/2 | 1/2 | 1/2 |
| typo_identifier | 2 | 2/2 | 2/2 | 1/2 |
