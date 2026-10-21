# RAG experiment: confirm-1.5b

Platform: macOS-26.5.2-arm64-arm-64bit-Mach-O  |  model: qwen2.5-coder-1.5b-instruct-q4_k_m.gguf (sha256 cc324af070c2)  |  llama-server: version: 0.7.0 (build 11669, commit 620d2225c)

Cases with every arm complete: 100 (of 100 started). Wilson 95% intervals in brackets.

## Per arm

| arm | n | parsed | abstained | located | unchanged | right_line | compiles | correct | exact_text | exact | correct_top | mean latency s | mean prompt tokens | mean context tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| A0_none | 100 | 100.0% [96.3, 100.0] | 0.0% [0.0, 3.7] | 57.0% [47.2, 66.3] | 43.0% [33.7, 52.8] | 47.0% [37.5, 56.7] | 29.0% [21.0, 38.5] | 26.0% [18.4, 35.4] | 25.0% [17.5, 34.3] | 23.0% [15.8, 32.2] | 26.0% [18.4, 35.4] | 2.3 | 996 | 0 |
| A1p_production | 100 | 100.0% [96.3, 100.0] | 0.0% [0.0, 3.7] | 58.0% [48.2, 67.2] | 39.0% [30.0, 48.8] | 44.0% [34.7, 53.8] | 22.0% [15.0, 31.1] | 21.0% [14.2, 30.0] | 18.0% [11.7, 26.7] | 16.0% [10.1, 24.4] | 21.0% [14.2, 30.0] | 4.3 | 2206 | 1194 |
| A3_examples | 100 | 100.0% [96.3, 100.0] | 0.0% [0.0, 3.7] | 65.0% [55.3, 73.6] | 31.0% [22.8, 40.6] | 58.0% [48.2, 67.2] | 43.0% [33.7, 52.8] | 43.0% [33.7, 52.8] | 43.0% [33.7, 52.8] | 39.0% [30.0, 48.8] | 43.0% [33.7, 52.8] | 2.5 | 1136 | 145 |
| A7_examples_reretrieve | 100 | 100.0% [96.3, 100.0] | 0.0% [0.0, 3.7] | 65.0% [55.3, 73.6] | 30.0% [21.9, 39.6] | 58.0% [48.2, 67.2] | 44.0% [34.7, 53.8] | 44.0% [34.7, 53.8] | 42.0% [32.8, 51.8] | 39.0% [30.0, 48.8] | 44.0% [34.7, 53.8] | 2.4 | 1136 | 146 |

## Paired comparison of `correct` (final outcome) (exact McNemar)

b = arm correct and baseline wrong, c = baseline correct and arm wrong.

| arm | vs | arm correct | base correct | b | c | p |
|---|---|---|---|---|---|---|
| A0_none | A1p_production | 26 | 21 | 9 | 4 | 0.2668 |
| A3_examples | A1p_production | 43 | 21 | 22 | 0 | 0.0000 |
| A7_examples_reretrieve | A1p_production | 44 | 21 | 23 | 0 | 0.0000 |
| A1p_production | A0_none | 21 | 26 | 4 | 9 | 0.2668 |
| A3_examples | A0_none | 43 | 26 | 19 | 2 | 0.0002 |
| A7_examples_reretrieve | A0_none | 44 | 26 | 20 | 2 | 0.0001 |

## Paired comparison of `correct@1` (exact McNemar)

b = arm correct and baseline wrong, c = baseline correct and arm wrong.

| arm | vs | arm correct | base correct | b | c | p |
|---|---|---|---|---|---|---|
| A0_none | A1p_production | 21 | 18 | 7 | 4 | 0.5488 |
| A3_examples | A1p_production | 38 | 18 | 23 | 3 | 0.0001 |
| A7_examples_reretrieve | A1p_production | 38 | 18 | 23 | 3 | 0.0001 |
| A1p_production | A0_none | 18 | 21 | 4 | 7 | 0.5488 |
| A3_examples | A0_none | 38 | 21 | 22 | 5 | 0.0015 |
| A7_examples_reretrieve | A0_none | 38 | 21 | 22 | 5 | 0.0015 |

## Retries with compiler feedback

`correct@k`: a correct patch within the first k attempts. A retry happens only after a patch production's sandbox would reject; the final `correct` equals `correct@N`.

| arm | n | correct@1 | correct@2 | correct@3 | mean n_calls | mean total latency s |
|---|---|---|---|---|---|---|
| A0_none | 100 | 21.0% [14.2, 30.0] | 25.0% [17.5, 34.3] | 26.0% [18.4, 35.4] | 2.48 | 5.5 |
| A1p_production | 100 | 18.0% [11.7, 26.7] | 19.0% [12.5, 27.8] | 21.0% [14.2, 30.0] | 2.61 | 11.0 |
| A3_examples | 100 | 38.0% [29.1, 47.8] | 43.0% [33.7, 52.8] | 43.0% [33.7, 52.8] | 2.19 | 5.4 |
| A7_examples_reretrieve | 100 | 38.0% [29.1, 47.8] | 42.0% [32.8, 51.8] | 44.0% [34.7, 53.8] | 2.20 | 5.4 |

## Gain from retries

| arm | correct@1 | correct@3 | gain | cases retried | retried and fixed |
|---|---|---|---|---|---|
| A0_none | 21/100 | 26/100 | +5 | 76 | 5 |
| A1p_production | 18/100 | 21/100 | +3 | 81 | 3 |
| A3_examples | 38/100 | 43/100 | +5 | 62 | 5 |
| A7_examples_reretrieve | 38/100 | 44/100 | +6 | 62 | 6 |

## `correct` by subset

| subset | n | A0_none | A1p_production | A3_examples | A7_examples_reretrieve |
|---|---|---|---|---|---|
| foreign syntax | 80 | 21/80 | 15/80 | 36/80 | 36/80 |
| classic | 20 | 5/20 | 6/20 | 7/20 | 8/20 |
| pair seen in train | 15 | 1/15 | 1/15 | 4/15 | 4/15 |
| novel pair | 85 | 25/85 | 20/85 | 39/85 | 40/85 |

## `correct` by kind

| kind | n | A0_none | A1p_production | A3_examples | A7_examples_reretrieve |
|---|---|---|---|---|---|
| delete_semicolon | 3 | 0/3 | 0/3 | 0/3 | 0/3 |
| drop_closing_brace | 3 | 0/3 | 0/3 | 0/3 | 0/3 |
| drop_paren | 3 | 1/3 | 1/3 | 1/3 | 1/3 |
| foreign_arrow_call (foreign syntax) | 3 | 3/3 | 2/3 | 3/3 | 3/3 |
| foreign_bool_literal (foreign syntax) | 3 | 2/3 | 2/3 | 2/3 | 2/3 |
| foreign_break (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 |
| foreign_case (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 |
| foreign_cast_c (foreign syntax) | 2 | 1/2 | 0/2 | 1/2 | 1/2 |
| foreign_cast_call (foreign syntax) | 3 | 0/3 | 0/3 | 0/3 | 0/3 |
| foreign_comment_hash (foreign syntax) | 4 | 0/4 | 0/4 | 0/4 | 0/4 |
| foreign_const_keyword (foreign syntax) | 2 | 0/2 | 0/2 | 1/2 | 1/2 |
| foreign_ctor_braces (foreign syntax) | 3 | 2/3 | 0/3 | 2/3 | 2/3 |
| foreign_ctor_call (foreign syntax) | 3 | 3/3 | 3/3 | 3/3 | 3/3 |
| foreign_decl_auto (foreign syntax) | 3 | 0/3 | 0/3 | 2/3 | 2/3 |
| foreign_decl_c (foreign syntax) | 3 | 1/3 | 0/3 | 2/3 | 2/3 |
| foreign_decl_const (foreign syntax) | 3 | 0/3 | 0/3 | 0/3 | 0/3 |
| foreign_decl_let (foreign syntax) | 3 | 0/3 | 0/3 | 0/3 | 0/3 |
| foreign_decl_let_mut (foreign syntax) | 3 | 0/3 | 0/3 | 0/3 | 0/3 |
| foreign_decl_var (foreign syntax) | 3 | 0/3 | 0/3 | 2/3 | 2/3 |
| foreign_decl_walrus (foreign syntax) | 3 | 0/3 | 0/3 | 2/3 | 2/3 |
| foreign_field_c (foreign syntax) | 3 | 0/3 | 0/3 | 1/3 | 1/3 |
| foreign_fn_keyword (foreign syntax) | 3 | 0/3 | 0/3 | 1/3 | 1/3 |
| foreign_for_c (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 |
| foreign_for_paren (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 |
| foreign_for_range (foreign syntax) | 1 | 1/1 | 1/1 | 1/1 | 1/1 |
| foreign_import (foreign syntax) | 3 | 0/3 | 0/3 | 0/3 | 0/3 |
| foreign_logic_word (foreign syntax) | 3 | 2/3 | 1/3 | 1/3 | 1/3 |
| foreign_module_dot (foreign syntax) | 3 | 3/3 | 1/3 | 2/3 | 2/3 |
| foreign_param_c (foreign syntax) | 3 | 1/3 | 0/3 | 2/3 | 2/3 |
| foreign_print (foreign syntax) | 3 | 0/3 | 2/3 | 2/3 | 2/3 |
| foreign_return_arrow (foreign syntax) | 3 | 0/3 | 0/3 | 2/3 | 2/3 |
| foreign_static_dot (foreign syntax) | 3 | 2/3 | 3/3 | 3/3 | 3/3 |
| foreign_switch (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 |
| foreign_ternary (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 |
| foreign_tostring (foreign syntax) | 2 | 0/2 | 0/2 | 1/2 | 1/2 |
| swap_return_type | 4 | 0/4 | 1/4 | 2/4 | 2/4 |
| swap_type | 3 | 3/3 | 3/3 | 3/3 | 3/3 |
| typo_identifier | 4 | 1/4 | 1/4 | 1/4 | 2/4 |
