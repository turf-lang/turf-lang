# RAG experiment: qwen3b-text

Platform: macOS-26.5.2-arm64-arm-64bit-Mach-O  |  model: qwen2.5-coder-3b-instruct-q4_k_m.gguf (sha256 724fb256bec1)  |  llama-server: version: 0.7.0 (build 11669, commit 620d2225c)

Cases with every arm complete: 72 (of 72 started). Wilson 95% intervals in brackets.

## Per arm

| arm | n | parsed | abstained | located | unchanged | right_line | compiles | correct | exact_text | exact | correct_top | mean latency s | mean prompt tokens | mean context tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| A0_none | 72 | 100.0% [94.9, 100.0] | 0.0% [0.0, 5.1] | 69.4% [58.0, 78.9] | 27.8% [18.8, 39.0] | 54.2% [42.7, 65.2] | 33.3% [23.5, 44.8] | 31.9% [22.3, 43.4] | 30.6% [21.1, 42.0] | 30.6% [21.1, 42.0] | 31.9% [22.3, 43.4] | 4.1 | 961 | 0 |
| A1p_production | 72 | 100.0% [94.9, 100.0] | 0.0% [0.0, 5.1] | 68.1% [56.6, 77.7] | 31.9% [22.3, 43.4] | 54.2% [42.7, 65.2] | 33.3% [23.5, 44.8] | 31.9% [22.3, 43.4] | 31.9% [22.3, 43.4] | 30.6% [21.1, 42.0] | 31.9% [22.3, 43.4] | 8.0 | 2151 | 1193 |
| A3_examples | 72 | 100.0% [94.9, 100.0] | 0.0% [0.0, 5.1] | 81.9% [71.5, 89.1] | 16.7% [9.8, 26.9] | 69.4% [58.0, 78.9] | 52.8% [41.4, 63.9] | 52.8% [41.4, 63.9] | 52.8% [41.4, 63.9] | 50.0% [38.7, 61.3] | 52.8% [41.4, 63.9] | 4.6 | 1104 | 146 |

## Paired comparison of `correct` (exact McNemar)

b = arm correct and baseline wrong, c = baseline correct and arm wrong.

| arm | vs | arm correct | base correct | b | c | p |
|---|---|---|---|---|---|---|
| A0_none | A1p_production | 23 | 23 | 6 | 6 | 1.0000 |
| A3_examples | A1p_production | 38 | 23 | 16 | 1 | 0.0003 |
| A1p_production | A0_none | 23 | 23 | 6 | 6 | 1.0000 |
| A3_examples | A0_none | 38 | 23 | 18 | 3 | 0.0015 |

## `correct` by subset

| subset | n | A0_none | A1p_production | A3_examples |
|---|---|---|---|---|
| foreign syntax | 61 | 20/61 | 20/61 | 32/61 |
| classic | 11 | 3/11 | 3/11 | 6/11 |
| pair seen in train | 14 | 3/14 | 4/14 | 5/14 |
| novel pair | 58 | 20/58 | 19/58 | 33/58 |

## `correct` by kind

| kind | n | A0_none | A1p_production | A3_examples |
|---|---|---|---|---|
| delete_semicolon | 1 | 0/1 | 0/1 | 0/1 |
| drop_closing_brace | 2 | 0/2 | 0/2 | 0/2 |
| drop_paren | 2 | 0/2 | 1/2 | 2/2 |
| foreign_array_brace (foreign syntax) | 2 | 2/2 | 1/2 | 2/2 |
| foreign_arrow_call (foreign syntax) | 2 | 1/2 | 0/2 | 0/2 |
| foreign_bool_literal (foreign syntax) | 2 | 1/2 | 1/2 | 2/2 |
| foreign_break (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 |
| foreign_case (foreign syntax) | 1 | 0/1 | 1/1 | 1/1 |
| foreign_cast_c (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| foreign_cast_call (foreign syntax) | 2 | 1/2 | 0/2 | 1/2 |
| foreign_comment_hash (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 |
| foreign_const_keyword (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| foreign_ctor_braces (foreign syntax) | 2 | 2/2 | 2/2 | 2/2 |
| foreign_ctor_call (foreign syntax) | 2 | 1/2 | 1/2 | 2/2 |
| foreign_decl_auto (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| foreign_decl_c (foreign syntax) | 2 | 1/2 | 1/2 | 2/2 |
| foreign_decl_const (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| foreign_decl_let (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| foreign_decl_let_mut (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| foreign_decl_var (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| foreign_decl_walrus (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| foreign_default (foreign syntax) | 1 | 1/1 | 1/1 | 1/1 |
| foreign_field_c (foreign syntax) | 2 | 1/2 | 1/2 | 0/2 |
| foreign_fn_keyword (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 |
| foreign_for_c (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| foreign_for_paren (foreign syntax) | 2 | 0/2 | 0/2 | 1/2 |
| foreign_for_range (foreign syntax) | 2 | 0/2 | 0/2 | 2/2 |
| foreign_import (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 |
| foreign_length (foreign syntax) | 2 | 2/2 | 2/2 | 2/2 |
| foreign_logic_word (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 |
| foreign_method_decl (foreign syntax) | 2 | 0/2 | 0/2 | 0/2 |
| foreign_module_dot (foreign syntax) | 2 | 2/2 | 1/2 | 2/2 |
| foreign_new_keyword (foreign syntax) | 1 | 0/1 | 0/1 | 1/1 |
| foreign_null (foreign syntax) | 2 | 2/2 | 1/2 | 2/2 |
| foreign_param_c (foreign syntax) | 2 | 1/2 | 1/2 | 2/2 |
| foreign_print (foreign syntax) | 2 | 0/2 | 2/2 | 2/2 |
| foreign_return_arrow (foreign syntax) | 2 | 0/2 | 2/2 | 2/2 |
| foreign_static_dot (foreign syntax) | 2 | 1/2 | 1/2 | 1/2 |
| foreign_switch (foreign syntax) | 1 | 1/1 | 1/1 | 1/1 |
| foreign_ternary (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 |
| foreign_tostring (foreign syntax) | 1 | 0/1 | 0/1 | 1/1 |
| swap_return_type | 2 | 1/2 | 0/2 | 0/2 |
| swap_type | 2 | 1/2 | 1/2 | 2/2 |
| typo_identifier | 2 | 1/2 | 1/2 | 2/2 |
