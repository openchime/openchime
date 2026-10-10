# Tuning a small summary model

How to teach a model too small to infer the summarizer's form from its
prompt (docs/SUMMARIES.md §2) to write it anyway, so that it can run in the
daemon on a small box (§6 there). Nothing in the daemon changes: the result is
one GGUF, pinned in `daemon/sum_fetch.h` like today's model, and the prompts,
the grammar and the checks stay as they are. The scripts are development
tools under `scripts/sumtune_*`; none run in the build or in CI.

**Why.** On the 30-span set (docs/MODELS.md), models of 1B or so hold the
form only because the grammar makes them, and what they write inside it is
copied lines, invented questions and bracketed names; the 2B is the smallest
that reads as a summary. A tenant box for the 2B needs 4 GB. A model of 0.8B
to 1.2B that had been shown our prompt and a correct answer a few thousand
times would hold the form on its own, keep the facts, and fit a box of
2.5 GB. Published work on exactly this (faithfulness tuning of 1B models)
cut their unfaithful summaries from two thirds to a quarter.

**What it will not do.** Judgement does not come from imitation at this
size: which topic matters most in a 24,000-word span, what belongs under
"Needs attention". Expect a tuned small model to pass on form, citations and
facts for small and medium spans, and to stay weaker than the 2B on large
ones.

## 1. Data: synthetic workspaces, teacher summaries

Real workspaces are private and few, so the chats are manufactured and the
summaries come from a verified teacher through the pipeline itself.

```
OPENCHIME_SUMMARY_API_KEY=… scripts/sumtune_synth.py https://openrouter.ai/api/v1 <model> work/synth 500
```

writes 500 workspaces in Slack's export layout, each a made-up company with
6 to 10 people, 3 channels, 3 to 8 days, threads, decisions, open questions,
an integration posting notifications. One large-model call a workspace;
cents each. A slice of a real workspace you may train on, loaded the same
way, makes the mix less tidy than synthetic chat alone.

Each workspace is loaded and summarized as a real one would be (SUMMARIES.md
§7), with a model that is **Verified** in MODELS.md as the engine (Llama 3.3
70B or Claude Haiku 5.5), and the trace kept:

```
for w in work/synth/ws*; do
  build/sumeval init $w/db.db && scripts/slack_to_db.py $w $w/db.db
  scripts/sumeval_set.py $w/db.db 609 $w/set.tsv
  OPENCHIME_SUMMARY_MODEL=meta-llama/llama-3.3-70b-instruct build/sumeval batch $w/db.db https://openrouter.ai/api/v1 $w/set.tsv $w/out 1500 all
done
scripts/sumtune_pairs.py work/pairs.jsonl work/synth/ws*/out
```

`sumtune_pairs.py` reads every traced call: the daemon's one system prompt,
the exact user prompt, the teacher's answer. A call whose answer the parser
stored in full is a pair with `kept` 1; a call that failed, or whose answer
the number and name checks cut, is a rejected pair, `kept` 0, for a later
preference pass. From the Llama 70B run of the 30-span set, 358 calls gave
321 kept pairs. 500 workspaces give on the order of 20,000.

## 2. Training

```
scripts/sumtune_train.py work/pairs.jsonl Qwen/Qwen3.5-0.8B work/adapter 2 32
```

A LoRA adapter (rank 32, all linear layers) by supervised fine-tuning with
TRL, loss on the answer only, the context at the daemon's 8,192 tokens so
every real prompt fits. 20,000 pairs of about 1,500 tokens for two epochs is
about 60 M tokens: under an hour on one H100, two to three on a 24 GB card.
Needs torch, transformers, peft, trl and datasets on the GPU machine.

A second pass, preference tuning on kept against rejected pairs (DPO in
TRL), is what the published results used to cut hallucination; it is the
step to add if the first pass holds the form but still invents.

## 3. Export and pin

```
LLAMACPP=../llama.cpp scripts/sumtune_export.sh Qwen/Qwen3.5-0.8B work/adapter work/export qwen3.5-0.8b-sum Q8_0
```

merges the adapter into the base, converts to GGUF with llama.cpp's script,
quantizes (Q8_0 for a model this small; Q4_K_M to save 0.3 GB), and prints
the five `OC_SUM_MODEL_*` lines with the file's size and SHA-256. Publish the
file where the daemon can fetch it, fill in the URL, and build with the header
as `scripts/sumpod.sh` does for a trial, or make it the default in
`daemon/sum_fetch.h`.

## 4. Verify

The same 30 spans on a 2-vCPU, 4 GB machine (SUMMARIES.md §7,
`scripts/sumpod_run.sh`), graded on the five points against the untuned base
and against the reference. A tuned model earns its row in MODELS.md the same
way as any other, and is **Verified** only when it meets the standard.

## What to watch for

- **Leakage**: the 30-span set must not be among the training workspaces.
  Synthetic workspaces cannot contain it; a real slice must leave it out.
- **Tidy data**: synthetic chat is cleaner than real chat. Mix in real
  messages where allowed, and keep the integration-post channels, which are
  where the small models failed most.
- **The prompt is the interface**: a pair is only valid for the prompt
  version it was made with. A new `SUM_PROMPT_VERSION` means new pairs.
- **Licence**: the base model's licence travels with the tuned file
  (Qwen3.5 is Apache 2.0).
