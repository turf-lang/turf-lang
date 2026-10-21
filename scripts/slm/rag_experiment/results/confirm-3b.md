# RAG experiment: confirm-3b

Platform: macOS-26.5.2-arm64-arm-64bit-Mach-O  |  model: qwen2.5-coder-3b-instruct-q4_k_m.gguf (sha256 724fb256bec1)  |  llama-server: version: 0.7.0 (build 11669, commit 620d2225c)

Cases with every arm complete: 100 (of 100 started). Wilson 95% intervals in brackets.

## Per arm

| arm | n | parsed | abstained | located | unchanged | right_line | compiles | correct | exact_text | exact | correct_top | mean latency s | mean prompt tokens | mean context tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| A0_none | 100 | 100.0% [96.3, 100.0] | 0.0% [0.0, 3.7] | 67.0% [57.3, 75.4] | 31.0% [22.8, 40.6] | 59.0% [49.2, 68.1] | 35.0% [26.4, 44.7] | 33.0% [24.6, 42.7] | 33.0% [24.6, 42.7] | 33.0% [24.6, 42.7] | 33.0% [24.6, 42.7] | 4.9 | 1004 | 0 |
| A1p_production | 100 | 100.0% [96.3, 100.0] | 0.0% [0.0, 3.7] | 63.0% [53.2, 71.8] | 37.0% [28.2, 46.8] | 54.0% [44.3, 63.4] | 35.0% [26.4, 44.7] | 34.0% [25.5, 43.7] | 32.0% [23.7, 41.7] | 30.0% [21.9, 39.6] | 34.0% [25.5, 43.7] | 9.3 | 2193 | 1194 |
| A3_examples | 100 | 100.0% [96.3, 100.0] | 0.0% [0.0, 3.7] | 77.0% [67.8, 84.2] | 22.0% [15.0, 31.1] | 70.0% [60.4, 78.1] | 59.0% [49.2, 68.1] | 59.0% [49.2, 68.1] | 56.0% [46.2, 65.3] | 54.0% [44.3, 63.4] | 59.0% [49.2, 68.1] | 5.2 | 1110 | 145 |
| A7_examples_reretrieve | 100 | 100.0% [96.3, 100.0] | 0.0% [0.0, 3.7] | 77.0% [67.8, 84.2] | 22.0% [15.0, 31.1] | 70.0% [60.4, 78.1] | 63.0% [53.2, 71.8] | 63.0% [53.2, 71.8] | 57.0% [47.2, 66.3] | 57.0% [47.2, 66.3] | 63.0% [53.2, 71.8] | 5.2 | 1109 | 145 |

## Paired comparison of `correct` (final outcome) (exact McNemar)

b = arm correct and baseline wrong, c = baseline correct and arm wrong.

| arm | vs | arm correct | base correct | b | c | p |
|---|---|---|---|---|---|---|
| A0_none | A1p_production | 33 | 34 | 8 | 9 | 1.0000 |
| A3_examples | A1p_production | 59 | 34 | 26 | 1 | 0.0000 |
| A7_examples_reretrieve | A1p_production | 63 | 34 | 30 | 1 | 0.0000 |
| A1p_production | A0_none | 34 | 33 | 9 | 8 | 1.0000 |
| A3_examples | A0_none | 59 | 33 | 27 | 1 | 0.0000 |
| A7_examples_reretrieve | A0_none | 63 | 33 | 30 | 0 | 0.0000 |

## Paired comparison of `correct@1` (exact McNemar)

b = arm correct and baseline wrong, c = baseline correct and arm wrong.

| arm | vs | arm correct | base correct | b | c | p |
|---|---|---|---|---|---|---|
| A0_none | A1p_production | 30 | 33 | 8 | 11 | 0.6476 |
| A3_examples | A1p_production | 55 | 33 | 25 | 3 | 0.0000 |
| A7_examples_reretrieve | A1p_production | 55 | 33 | 25 | 3 | 0.0000 |
| A1p_production | A0_none | 33 | 30 | 11 | 8 | 0.6476 |
| A3_examples | A0_none | 55 | 30 | 28 | 3 | 0.0000 |
| A7_examples_reretrieve | A0_none | 55 | 30 | 28 | 3 | 0.0000 |

## Retries with compiler feedback

`correct@k`: a correct patch within the first k attempts. A retry happens only after a patch production's sandbox would reject; the final `correct` equals `correct@N`.

| arm | n | correct@1 | correct@2 | correct@3 | mean n_calls | mean total latency s |
|---|---|---|---|---|---|---|
| A0_none | 100 | 30.0% [21.9, 39.6] | 33.0% [24.6, 42.7] | 33.0% [24.6, 42.7] | 2.33 | 11.1 |
| A1p_production | 100 | 33.0% [24.6, 42.7] | 34.0% [25.5, 43.7] | 34.0% [25.5, 43.7] | 2.31 | 21.4 |
| A3_examples | 100 | 55.0% [45.2, 64.4] | 59.0% [49.2, 68.1] | 59.0% [49.2, 68.1] | 1.86 | 9.8 |
| A7_examples_reretrieve | 100 | 55.0% [45.2, 64.4] | 60.0% [50.2, 69.1] | 63.0% [53.2, 71.8] | 1.85 | 9.6 |

## Gain from retries

| arm | correct@1 | correct@3 | gain | cases retried | retried and fixed |
|---|---|---|---|---|---|
| A0_none | 30/100 | 33/100 | +3 | 68 | 3 |
| A1p_production | 33/100 | 34/100 | +1 | 66 | 1 |
| A3_examples | 55/100 | 59/100 | +4 | 45 | 4 |
| A7_examples_reretrieve | 55/100 | 63/100 | +8 | 45 | 8 |

## `correct` by subset

| subset | n | A0_none | A1p_production | A3_examples | A7_examples_reretrieve |
|---|---|---|---|---|---|
| foreign syntax | 80 | 24/80 | 26/80 | 47/80 | 51/80 |
| classic | 20 | 9/20 | 8/20 | 12/20 | 12/20 |
| pair seen in train | 15 | 2/15 | 5/15 | 8/15 | 8/15 |
| novel pair | 85 | 31/85 | 29/85 | 51/85 | 55/85 |

## `correct` by kind

| kind | n | A0_none | A1p_production | A3_examples | A7_examples_reretrieve |
|---|---|---|---|---|---|
| delete_semicolon | 3 | 1/3 | 0/3 | 1/3 | 1/3 |
| drop_closing_brace | 3 | 0/3 | 0/3 | 0/3 | 0/3 |
| drop_paren | 3 | 2/3 | 2/3 | 2/3 | 2/3 |
| foreign_arrow_call (foreign syntax) | 3 | 3/3 | 1/3 | 2/3 | 3/3 |
| foreign_bool_literal (foreign syntax) | 3 | 2/3 | 1/3 | 3/3 | 3/3 |
| foreign_break (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 |
| foreign_case (foreign syntax) | 1 | 0/1 | 0/1 | 1/1 | 1/1 |
| foreign_cast_c (foreign syntax) | 2 | 1/2 | 1/2 | 1/2 | 2/2 |
| foreign_cast_call (foreign syntax) | 3 | 1/3 | 0/3 | 1/3 | 1/3 |
| foreign_comment_hash (foreign syntax) | 4 | 0/4 | 0/4 | 1/4 | 1/4 |
| foreign_const_keyword (foreign syntax) | 2 | 0/2 | 0/2 | 1/2 | 1/2 |
| foreign_ctor_braces (foreign syntax) | 3 | 1/3 | 2/3 | 2/3 | 2/3 |
| foreign_ctor_call (foreign syntax) | 3 | 3/3 | 3/3 | 3/3 | 3/3 |
| foreign_decl_auto (foreign syntax) | 3 | 0/3 | 0/3 | 0/3 | 0/3 |
| foreign_decl_c (foreign syntax) | 3 | 2/3 | 2/3 | 3/3 | 3/3 |
| foreign_decl_const (foreign syntax) | 3 | 0/3 | 0/3 | 1/3 | 1/3 |
| foreign_decl_let (foreign syntax) | 3 | 0/3 | 0/3 | 1/3 | 1/3 |
| foreign_decl_let_mut (foreign syntax) | 3 | 0/3 | 0/3 | 0/3 | 1/3 |
| foreign_decl_var (foreign syntax) | 3 | 0/3 | 0/3 | 1/3 | 1/3 |
| foreign_decl_walrus (foreign syntax) | 3 | 0/3 | 0/3 | 2/3 | 2/3 |
| foreign_field_c (foreign syntax) | 3 | 0/3 | 1/3 | 2/3 | 2/3 |
| foreign_fn_keyword (foreign syntax) | 3 | 1/3 | 1/3 | 1/3 | 1/3 |
| foreign_for_c (foreign syntax) | 1 | 0/1 | 0/1 | 1/1 | 1/1 |
| foreign_for_paren (foreign syntax) | 1 | 0/1 | 0/1 | 1/1 | 1/1 |
| foreign_for_range (foreign syntax) | 1 | 0/1 | 1/1 | 1/1 | 1/1 |
| foreign_import (foreign syntax) | 3 | 0/3 | 0/3 | 0/3 | 0/3 |
| foreign_logic_word (foreign syntax) | 3 | 1/3 | 1/3 | 1/3 | 1/3 |
| foreign_module_dot (foreign syntax) | 3 | 3/3 | 3/3 | 3/3 | 3/3 |
| foreign_param_c (foreign syntax) | 3 | 2/3 | 1/3 | 3/3 | 3/3 |
| foreign_print (foreign syntax) | 3 | 0/3 | 2/3 | 2/3 | 3/3 |
| foreign_return_arrow (foreign syntax) | 3 | 0/3 | 3/3 | 3/3 | 3/3 |
| foreign_static_dot (foreign syntax) | 3 | 3/3 | 3/3 | 3/3 | 3/3 |
| foreign_switch (foreign syntax) | 1 | 0/1 | 0/1 | 1/1 | 1/1 |
| foreign_ternary (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 |
| foreign_tostring (foreign syntax) | 2 | 1/2 | 0/2 | 2/2 | 2/2 |
| swap_return_type | 4 | 0/4 | 0/4 | 2/4 | 2/4 |
| swap_type | 3 | 3/3 | 2/3 | 3/3 | 3/3 |
| typo_identifier | 4 | 3/4 | 4/4 | 4/4 | 4/4 |
