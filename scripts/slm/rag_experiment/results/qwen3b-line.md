# RAG experiment: qwen3b-line

Platform: macOS-26.5.2-arm64-arm-64bit-Mach-O  |  model: qwen2.5-coder-3b-instruct-q4_k_m.gguf (sha256 724fb256bec1)  |  llama-server: version: 0.7.0 (build 11669, commit 620d2225c)

Cases with every arm complete: 72 (of 72 started). Wilson 95% intervals in brackets.

## Per arm

| arm | n | parsed | abstained | right_line | compiles | correct | exact_text | exact | mean latency s | mean prompt tokens | mean context tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|
| A0_none | 72 | 100.0% [94.9, 100.0] | 0.0% [0.0, 5.1] | 77.8% [66.9, 85.8] | 29.2% [19.9, 40.5] | 27.8% [18.8, 39.0] | 23.6% [15.3, 34.6] | 23.6% [15.3, 34.6] | 4.2 | 1094 | 0 |
| A1p_production | 72 | 100.0% [94.9, 100.0] | 0.0% [0.0, 5.1] | 77.8% [66.9, 85.8] | 23.6% [15.3, 34.6] | 23.6% [15.3, 34.6] | 20.8% [13.1, 31.6] | 20.8% [13.1, 31.6] | 8.2 | 2284 | 1193 |
| A3_examples | 72 | 100.0% [94.9, 100.0] | 0.0% [0.0, 5.1] | 79.2% [68.4, 86.9] | 38.9% [28.5, 50.4] | 38.9% [28.5, 50.4] | 44.4% [33.5, 55.9] | 36.1% [26.0, 47.6] | 4.7 | 1237 | 146 |

## Paired comparison of `correct` (exact McNemar)

b = arm correct and baseline wrong, c = baseline correct and arm wrong.

| arm | vs | arm correct | base correct | b | c | p |
|---|---|---|---|---|---|---|
| A0_none | A1p_production | 20 | 17 | 7 | 4 | 0.5488 |
| A3_examples | A1p_production | 28 | 17 | 13 | 2 | 0.0074 |
| A1p_production | A0_none | 17 | 20 | 4 | 7 | 0.5488 |
| A3_examples | A0_none | 28 | 20 | 12 | 4 | 0.0768 |

## `correct` by subset

| subset | n | A0_none | A1p_production | A3_examples |
|---|---|---|---|---|
| foreign syntax | 61 | 15/61 | 12/61 | 23/61 |
| classic | 11 | 5/11 | 5/11 | 5/11 |
| pair seen in train | 14 | 4/14 | 2/14 | 3/14 |
| novel pair | 58 | 16/58 | 15/58 | 25/58 |

## `correct` by kind

| kind | n | A0_none | A1p_production | A3_examples |
|---|---|---|---|---|
| delete_semicolon | 1 | 0/1 | 1/1 | 1/1 |
| drop_closing_brace | 2 | 1/2 | 0/2 | 0/2 |
| drop_paren | 2 | 2/2 | 2/2 | 2/2 |
| foreign_array_brace (foreign syntax) | 2 | 2/2 | 0/2 | 2/2 |
| foreign_arrow_call (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 |
| foreign_bool_literal (foreign syntax) | 2 | 1/2 | 0/2 | 1/2 |
| foreign_break (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 |
| foreign_case (foreign syntax) | 1 | 0/1 | 0/1 | 1/1 |
| foreign_cast_c (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| foreign_cast_call (foreign syntax) | 2 | 0/2 | 0/2 | 1/2 |
| foreign_comment_hash (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 |
| foreign_const_keyword (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| foreign_ctor_braces (foreign syntax) | 2 | 2/2 | 2/2 | 1/2 |
| foreign_ctor_call (foreign syntax) | 2 | 1/2 | 2/2 | 2/2 |
| foreign_decl_auto (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| foreign_decl_c (foreign syntax) | 2 | 1/2 | 1/2 | 1/2 |
| foreign_decl_const (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| foreign_decl_let (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| foreign_decl_let_mut (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| foreign_decl_var (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| foreign_decl_walrus (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| foreign_default (foreign syntax) | 1 | 0/1 | 0/1 | 1/1 |
| foreign_field_c (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 |
| foreign_fn_keyword (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 |
| foreign_for_c (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| foreign_for_paren (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 |
| foreign_for_range (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 |
| foreign_import (foreign syntax) | 2 | 1/2 | 1/2 | 1/2 |
| foreign_length (foreign syntax) | 2 | 2/2 | 2/2 | 2/2 |
| foreign_logic_word (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 |
| foreign_method_decl (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 |
| foreign_module_dot (foreign syntax) | 2 | 2/2 | 2/2 | 2/2 |
| foreign_new_keyword (foreign syntax) | 1 | 0/1 | 0/1 | 1/1 |
| foreign_null (foreign syntax) | 2 | 1/2 | 0/2 | 2/2 |
| foreign_param_c (foreign syntax) | 2 | 0/2 | 1/2 | 2/2 |
| foreign_print (foreign syntax) | 2 | 1/2 | 0/2 | 1/2 |
| foreign_return_arrow (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 |
| foreign_static_dot (foreign syntax) | 2 | 1/2 | 1/2 | 1/2 |
| foreign_switch (foreign syntax) | 1 | 0/1 | 0/1 | 1/1 |
| foreign_ternary (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| foreign_tostring (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| swap_return_type | 2 | 1/2 | 0/2 | 0/2 |
| swap_type | 2 | 0/2 | 1/2 | 1/2 |
| typo_identifier | 2 | 1/2 | 1/2 | 1/2 |
