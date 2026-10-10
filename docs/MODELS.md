# Summary models tried

Which models the summarizer (docs/SUMMARIES.md) has been run with, how each
did, and what to know before choosing one. The prompt is the same for every
model; nothing in it is written for one (§6 there). A model is **verified** when
it completed every span of the evaluation set with the current prompt version and
its summaries read as the standard asks (§1 there), graded by hand.

**How a run is made.** The evaluation set is 30 spans of one real workspace
(10 small, 10 medium, 10 large; `scripts/sumeval_set.py`), run cold through the
real pipeline with `sumeval batch` (§7 there) against the hosted engine, a
chat-completions URL. Hosted runs here went through OpenRouter
(`https://openrouter.ai/api/v1`), which fronts the model's own vendor or
third-party providers under one API and key. Numbers are from those runs; the
wall time of a call is the provider's, not the pipeline's, and varies by the hour.
Prompt versions are `SUM_PROMPT_VERSION` in `daemon/sum_core.h`; results under
an older version are not comparable with the current one.

## Hosted, through OpenRouter

| Model (OpenRouter name) | Prompt | Spans done | Verdict | Notes |
|---|---|---|---|---|
| `meta-llama/llama-3.3-70b-instruct` | s15 | 30/30, twice | **Verified** | The reference. About 5 s a call, 10 s for a large span's final call (the provider's time; it has ranged to 60 s). Faithful and specific on human channels; mean 118 words. Two of 401 chunk calls in one run came back as token loops from the provider: the first run kept the pieces and a second pass finished both spans in 3 and 1 calls; the re-test, with the second ask on a runaway, had none. Writes bare citations and titles without `##` now and then, which the parser accepts. $0.09 for the set. |
| `openai/gpt-4.1` | s15 | 30/30, twice | **Verified** | One pass each time. OpenRouter holds a new account to 20 requests a minute on this model: about 70 of 404 requests were answered 429 and sent again after the pause, 8 minutes waited in all, none failing a span. Specific and well cited; mean 156 words. About ten times Llama 70B per token: $1.00 for the set. |
| `anthropic/claude-haiku-5.5` | s15 | 30/30, twice | **Verified** | Reasons before answering when it finds a prompt worth it; the engine learns that on the first such call and sends `reasoning_effort: "none"` from then on (SUMMARIES.md §6). Content the richest tried, with amounts, names and reasons kept, mean 172 words. Writes past the total on half the spans: the shorten pass gains a few words, the trim does the rest. One runaway in 412 calls, asked again and answered. On s14 it failed 7 of 30 by a token cap sized to the total, which s15 removed. Priced below Llama 70B: $0.11 for the set. |
| `openai/gpt-4.1-mini` | s15 | 30/30, twice | **Verified** | Fastest tried, under 2 s a call. Writes past the total on half the spans and is shortened once, then trimmed; on s14 it failed 9 of 30 by the token cap. Specific and cited; mean 179 words. $0.21 for the set. |

**Small models through the hosted path** (candidates for the local engine,
run the same way; without the grammar the local engine holds them to, so a
form failure here says less than it would locally):

| Model (OpenRouter name) | Prompt | Spans done | Notes |
|---|---|---|---|
| `ibm-granite/granite-4.0-h-micro` (3B) | s15 | 30/30 | Holds the form; the thinnest content tried, mean 75 words, 473 sentences cut by the checks for numbers or names the lines do not have. Overviews are sometimes a line's label ("integration: Attachment provided"). |

Not tried: Google Gemini, xAI Grok, the larger Anthropic and OpenAI models.

## Local, in the daemon (llama.cpp)

Run as a tenant would run them: the real daemon built for the model
(`scripts/sumpod.sh`), on a 2-vCPU, 4 GB machine (RunPod `cpu3c`, the size
of the smallest tenant box), fetching and checking its GGUF at startup and
asked for the 30 spans over the wire by a client (`scripts/sumpod_run.sh`).
Prompt s15. Times are for the whole span cold, every chunk call included; a
request a tenant's daemon pre-mapped waits for the final call only.

| Model | Spans done | Set time | Peak resident | Verdict | Notes |
|---|---|---|---|---|---|
| Qwen3.5 2B, Q4_K_M (the pinned default) | 29/30 | 5 h 55 min | 2.18 GB | Near the standard; the only small model whose summaries read as summaries | Keeps names, dates, numbers and decisions on small and medium spans; large spans keep the specifics but rank them loosely. Misattributes now and then (a reported bug written as if the reporter caused it) and pads "Needs attention" with statements on about a third of spans. One span not in the form; one runaway, asked again and answered. Small spans 55 to 150 s, medium 80 to 240 s, large 10 to 85 min. |
| Qwen3.5 0.8B, Q8_0 | 28/30 | 3 h 6 min | 1.22 GB | Below the standard | Keeps numbers and ids better than the 1B class but copies labels and fragments into titles and overviews, invents on several spans, repeats one notification four times, and was slower than LFM2.5. The tuning candidate by size (docs/TUNING.md). |
| LFM2.5 1.2B, Q8_0 | 29/30 | 1 h 50 min | 1.39 GB | Below the standard | The fastest by far, 2 to 4 times the others. Writes names in square brackets, citations into titles, placeholders ("[Name] confirm schedule"), and three-word lines with no facts on large spans. The tuning candidate by speed. |
| Gemma 3 1B, Q8_0 | 30/30 | 2 h 33 min | 1.48 GB | Below the standard | Overviews are a copied line with the author's name in front; repeats a topic title five times on one span; degenerates on the largest. Keeps some ids and amounts. |
| Llama 3.2 1B, Q8_0 | 29/30 | 2 h 53 min | 1.66 GB | Below the standard | Copies lines with field labels as sentences, lists compliments as actions, repeats one garbled question five times. |
| Qwen3 1.7B, Q4_K_M | 28/30 | 4 h 10 min | 2.44 GB | Below the standard | The older generation: more memory than Qwen3.5 2B and worse. Copies the note labels ("(event) deal: …") into topic text, writes "Name: did X" fragments as details, overviews on bot spans are the notification itself. Two spans failed on a notes runaway that the second ask did not cure. |
| Granite 4.0 H 1B, Q4_K_M | 30/30 | 4 h 35 min | 1.79 GB | Below the standard, the weakest | Holds the form only by the grammar: one overview is the prompt's own placeholder text, topics are note labels and copied lines, details carry the prompt's section names ("Needs attention: No. No question asked. More topics: ["), amounts drift ($6,000 becomes $6,00). Slow reading on CPU (about 20 tokens a second), 4 h 35 min for the set. |

The local engine runs beside the daemon on a box of 4 GB, so a candidate is
one whose GGUF and context fit about 2.5 GB (§7 there). Below 2B, no model
off the shelf holds the form by itself: the grammar makes the answer parse,
and what is inside it is copied lines and invented questions. Teaching one
the form is docs/TUNING.md.

## What to tell a user choosing a hosted model

- Any OpenAI-style chat-completions API works; the request is `model`,
  `messages`, `temperature` 0 and `max_tokens`, nothing else.
- The model is named by its provider's name, exactly (`OPENCHIME_SUMMARY_MODEL`).
  Changing it remakes every summary.
- A model that reasons before answering is told not to at startup, through
  whichever of the known fields its API honours (SUMMARIES.md §6, one log line
  says which). Some cannot be told (OpenAI's GPT-5 family on OpenRouter
  answers "reasoning is mandatory"; Qwen3-14B's provider there ignores every
  field), and summaries stay off with that reason.
- A model that writes past the total it was asked for is not failed for it
  (since prompt s15): it is asked once to write the summary again shorter,
  and the result is trimmed to the total. Every model overshoots a word limit
  some of the time, the larger commercial ones most.
- A provider's rate limit (429) or outage (5xx) is waited out: the same
  request is sent again after the pause the server names or a doubling one,
  up to five times and under three minutes in all, one log line a wait. A span
  that still fails keeps the pieces it finished for the next try.
- A large span's final call takes 15 to 40 s on a 70B-class API model; the
  chunk calls before it are made ahead of time for the last 7 days (§4, §5
  there) and on demand for older ranges.

## Testing not complete for

GPT-5 mini, Qwen3 14B, Qwen3 8B, Mistral Large 2512 (hosted);
Gemma 3 4B, Ministral 3B, Llama 3.2 3B (small models through the hosted path);
Granite 4.0 H Micro 3B, Gemma 3 4B, Ministral 3B, Llama 3.2 3B, Gemma 4 E2B (local, 4 GB).
