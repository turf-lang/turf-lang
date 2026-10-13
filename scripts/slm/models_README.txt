TurfLang --smart: put your model here
=====================================

`turf file.tr --smart` asks a small language model, running on your own
machine, to explain a compile error and suggest a fix. This folder is where
that model lives. No model comes with TurfLang: you choose one.

Put exactly ONE .gguf file in this folder.

  * none:      --smart says there is no model and shows the compiler's own
               diagnostics. Nothing else changes.
  * one:       --smart uses it. The file's name does not matter.
  * more than  --smart says it found several and uses none of them, because it
    one:       cannot know which you meant. Remove all but one.

Recommended (the model TurfLang is developed and continuously tested with):

  qwen2.5-coder-1.5b-instruct-q4_k_m.gguf     about 1 GB on disk,
                                              about 1.5 GB of RAM at its peak
                                              while it works on an error

  https://huggingface.co/Qwen/Qwen2.5-Coder-1.5B-Instruct-GGUF

Any GGUF model llama.cpp can run will load, so with more memory you can try a
larger one. The question is put to the model in the chat format stored in its
own file (Llama, Mistral, Gemma, Phi and ChatML models each get theirs); a model
whose file has no chat format llama.cpp knows is asked in ChatML, the format the
recommended model uses. TurfLang's prompts and checks are tuned on the model
above; another model may give worse suggestions, or none, and a small one may
not follow the output format at all. Every suggestion is checked by the
compiler before you see it, whichever model made it.

A model is only read when a compile fails with --smart; programs that compile
never load it. Installing a new version of TurfLang or uninstalling it with
install.sh / install.ps1 leaves this folder alone.
