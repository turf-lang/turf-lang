# RAG experiment: confirm-7b

Platform: macOS-26.5.2-arm64-arm-64bit-Mach-O  |  model: qwen2.5-coder-7b-instruct-q4_k_m.gguf (sha256 509287f78cb4)  |  llama-server: version: 0.7.0 (build 11669, commit 620d2225c)

Cases with every arm complete: 100 (of 100 started). Wilson 95% intervals in brackets.

## Per arm

| arm | n | parsed | abstained | located | unchanged | right_line | compiles | correct | exact_text | exact | correct_top | mean latency s | mean prompt tokens | mean context tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| A0_none | 100 | 99.0% [94.6, 99.8] | 0.0% [0.0, 3.7] | 82.0% [73.3, 88.3] | 17.0% [10.9, 25.5] | 69.0% [59.4, 77.2] | 45.0% [35.6, 54.8] | 42.0% [32.8, 51.8] | 41.0% [31.9, 50.8] | 41.0% [31.9, 50.8] | 42.0% [32.8, 51.8] | 11.2 | 998 | 0 |
| A1p_production | 100 | 100.0% [96.3, 100.0] | 0.0% [0.0, 3.7] | 73.0% [63.6, 80.7] | 26.0% [18.4, 35.4] | 63.0% [53.2, 71.8] | 48.0% [38.5, 57.7] | 47.0% [37.5, 56.7] | 43.0% [33.7, 52.8] | 43.0% [33.7, 52.8] | 47.0% [37.5, 56.7] | 20.9 | 2170 | 1194 |
| A3_examples | 100 | 99.0% [94.6, 99.8] | 0.0% [0.0, 3.7] | 90.0% [82.6, 94.5] | 9.0% [4.8, 16.2] | 81.0% [72.2, 87.5] | 74.0% [64.6, 81.6] | 73.0% [63.6, 80.7] | 72.0% [62.5, 79.9] | 69.0% [59.4, 77.2] | 73.0% [63.6, 80.7] | 11.8 | 1085 | 145 |
| A7_examples_reretrieve | 100 | 99.0% [94.6, 99.8] | 0.0% [0.0, 3.7] | 90.0% [82.6, 94.5] | 9.0% [4.8, 16.2] | 81.0% [72.2, 87.5] | 74.0% [64.6, 81.6] | 73.0% [63.6, 80.7] | 71.0% [61.5, 79.0] | 69.0% [59.4, 77.2] | 73.0% [63.6, 80.7] | 11.7 | 1084 | 144 |

## Paired comparison of `correct` (final outcome) (exact McNemar)

b = arm correct and baseline wrong, c = baseline correct and arm wrong.

| arm | vs | arm correct | base correct | b | c | p |
|---|---|---|---|---|---|---|
| A0_none | A1p_production | 42 | 47 | 8 | 13 | 0.3833 |
| A3_examples | A1p_production | 73 | 47 | 29 | 3 | 0.0000 |
| A7_examples_reretrieve | A1p_production | 73 | 47 | 28 | 2 | 0.0000 |
| A1p_production | A0_none | 47 | 42 | 13 | 8 | 0.3833 |
| A3_examples | A0_none | 73 | 42 | 33 | 2 | 0.0000 |
| A7_examples_reretrieve | A0_none | 73 | 42 | 34 | 3 | 0.0000 |

## Paired comparison of `correct@1` (exact McNemar)

b = arm correct and baseline wrong, c = baseline correct and arm wrong.

| arm | vs | arm correct | base correct | b | c | p |
|---|---|---|---|---|---|---|
| A0_none | A1p_production | 40 | 46 | 6 | 12 | 0.2379 |
| A3_examples | A1p_production | 71 | 46 | 27 | 2 | 0.0000 |
| A7_examples_reretrieve | A1p_production | 71 | 46 | 27 | 2 | 0.0000 |
| A1p_production | A0_none | 46 | 40 | 12 | 6 | 0.2379 |
| A3_examples | A0_none | 71 | 40 | 33 | 2 | 0.0000 |
| A7_examples_reretrieve | A0_none | 71 | 40 | 33 | 2 | 0.0000 |

## Retries with compiler feedback

`correct@k`: a correct patch within the first k attempts. A retry happens only after a patch production's sandbox would reject; the final `correct` equals `correct@N`.

| arm | n | correct@1 | correct@2 | correct@3 | mean n_calls | mean total latency s |
|---|---|---|---|---|---|---|
| A0_none | 100 | 40.0% [30.9, 49.8] | 41.0% [31.9, 50.8] | 42.0% [32.8, 51.8] | 2.11 | 22.9 |
| A1p_production | 100 | 46.0% [36.6, 55.7] | 47.0% [37.5, 56.7] | 47.0% [37.5, 56.7] | 2.05 | 42.6 |
| A3_examples | 100 | 71.0% [61.5, 79.0] | 72.0% [62.5, 79.9] | 73.0% [63.6, 80.7] | 1.53 | 17.8 |
| A7_examples_reretrieve | 100 | 71.0% [61.5, 79.0] | 73.0% [63.6, 80.7] | 73.0% [63.6, 80.7] | 1.52 | 17.5 |

## Gain from retries

| arm | correct@1 | correct@3 | gain | cases retried | retried and fixed |
|---|---|---|---|---|---|
| A0_none | 40/100 | 42/100 | +2 | 56 | 2 |
| A1p_production | 46/100 | 47/100 | +1 | 53 | 1 |
| A3_examples | 71/100 | 73/100 | +2 | 27 | 2 |
| A7_examples_reretrieve | 71/100 | 73/100 | +2 | 27 | 2 |

## `correct` by subset

| subset | n | A0_none | A1p_production | A3_examples | A7_examples_reretrieve |
|---|---|---|---|---|---|
| foreign syntax | 80 | 32/80 | 38/80 | 63/80 | 63/80 |
| classic | 20 | 10/20 | 9/20 | 10/20 | 10/20 |
| pair seen in train | 15 | 2/15 | 7/15 | 7/15 | 7/15 |
| novel pair | 85 | 40/85 | 40/85 | 66/85 | 66/85 |

## `correct` by kind

| kind | n | A0_none | A1p_production | A3_examples | A7_examples_reretrieve |
|---|---|---|---|---|---|
| delete_semicolon | 3 | 0/3 | 1/3 | 0/3 | 0/3 |
| drop_closing_brace | 3 | 0/3 | 0/3 | 0/3 | 0/3 |
| drop_paren | 3 | 2/3 | 1/3 | 1/3 | 1/3 |
| foreign_arrow_call (foreign syntax) | 3 | 2/3 | 2/3 | 2/3 | 2/3 |
| foreign_bool_literal (foreign syntax) | 3 | 3/3 | 3/3 | 3/3 | 3/3 |
| foreign_break (foreign syntax) | 1 | 0/1 | 0/1 | 0/1 | 0/1 |
| foreign_case (foreign syntax) | 1 | 1/1 | 0/1 | 1/1 | 1/1 |
| foreign_cast_c (foreign syntax) | 2 | 2/2 | 2/2 | 2/2 | 2/2 |
| foreign_cast_call (foreign syntax) | 3 | 0/3 | 0/3 | 2/3 | 2/3 |
| foreign_comment_hash (foreign syntax) | 4 | 2/4 | 1/4 | 3/4 | 3/4 |
| foreign_const_keyword (foreign syntax) | 2 | 0/2 | 0/2 | 1/2 | 1/2 |
| foreign_ctor_braces (foreign syntax) | 3 | 1/3 | 2/3 | 3/3 | 3/3 |
| foreign_ctor_call (foreign syntax) | 3 | 3/3 | 3/3 | 3/3 | 3/3 |
| foreign_decl_auto (foreign syntax) | 3 | 0/3 | 0/3 | 3/3 | 3/3 |
| foreign_decl_c (foreign syntax) | 3 | 2/3 | 2/3 | 3/3 | 3/3 |
| foreign_decl_const (foreign syntax) | 3 | 0/3 | 0/3 | 2/3 | 2/3 |
| foreign_decl_let (foreign syntax) | 3 | 0/3 | 0/3 | 0/3 | 0/3 |
| foreign_decl_let_mut (foreign syntax) | 3 | 1/3 | 0/3 | 3/3 | 3/3 |
| foreign_decl_var (foreign syntax) | 3 | 0/3 | 0/3 | 2/3 | 2/3 |
| foreign_decl_walrus (foreign syntax) | 3 | 1/3 | 0/3 | 3/3 | 2/3 |
| foreign_field_c (foreign syntax) | 3 | 1/3 | 3/3 | 2/3 | 2/3 |
| foreign_fn_keyword (foreign syntax) | 3 | 1/3 | 1/3 | 1/3 | 1/3 |
| foreign_for_c (foreign syntax) | 1 | 0/1 | 0/1 | 1/1 | 1/1 |
| foreign_for_paren (foreign syntax) | 1 | 0/1 | 0/1 | 1/1 | 1/1 |
| foreign_for_range (foreign syntax) | 1 | 1/1 | 1/1 | 1/1 | 1/1 |
| foreign_import (foreign syntax) | 3 | 0/3 | 0/3 | 0/3 | 0/3 |
| foreign_logic_word (foreign syntax) | 3 | 2/3 | 2/3 | 3/3 | 3/3 |
| foreign_module_dot (foreign syntax) | 3 | 3/3 | 3/3 | 3/3 | 3/3 |
| foreign_param_c (foreign syntax) | 3 | 3/3 | 3/3 | 3/3 | 3/3 |
| foreign_print (foreign syntax) | 3 | 0/3 | 3/3 | 2/3 | 3/3 |
| foreign_return_arrow (foreign syntax) | 3 | 0/3 | 3/3 | 3/3 | 3/3 |
| foreign_static_dot (foreign syntax) | 3 | 3/3 | 3/3 | 3/3 | 3/3 |
| foreign_switch (foreign syntax) | 1 | 0/1 | 0/1 | 1/1 | 1/1 |
| foreign_ternary (foreign syntax) | 1 | 0/1 | 0/1 | 1/1 | 1/1 |
| foreign_tostring (foreign syntax) | 2 | 0/2 | 1/2 | 2/2 | 2/2 |
| swap_return_type | 4 | 2/4 | 1/4 | 2/4 | 2/4 |
| swap_type | 3 | 3/3 | 2/3 | 3/3 | 3/3 |
| typo_identifier | 4 | 3/4 | 4/4 | 4/4 | 4/4 |
