#!/usr/bin/env python3
"""Fine-tune a small model on summary pairs with a LoRA adapter (docs/TUNING.md).

    scripts/sumtune_train.py <pairs.jsonl> <base model> <out dir> [epochs] [rank]

Supervised fine-tuning on the pairs scripts/sumtune_pairs.py wrote: the
daemon's system prompt and the exact user prompt as the input, the
teacher's answer as the target, loss on the answer only. Pairs with `kept`
0 are left out here (they are the rejected side of a later preference pass).
The base is a Hugging Face model name or path, e.g. Qwen/Qwen3.5-0.8B; the
adapter and tokenizer go to <out dir>, and scripts/sumtune_export.sh turns
them into a GGUF the daemon can pin.

Needs: torch, transformers, peft, trl, datasets (pip install, on the GPU
machine, not here). One 24 GB card or an H100; a 0.8B in bf16 fits either.
Development only.
"""
import json
import sys


def main(pairs, base, out, epochs=2, rank=32):
    import torch
    from datasets import Dataset
    from peft import LoraConfig
    from transformers import AutoModelForCausalLM, AutoTokenizer
    from trl import SFTConfig, SFTTrainer

    rows = [json.loads(l) for l in open(pairs)]
    rows = [r for r in rows if r.get("kept")]
    if not rows:
        raise SystemExit("no kept pairs")
    tok = AutoTokenizer.from_pretrained(base)
    if tok.pad_token is None:
        tok.pad_token = tok.eos_token

    def to_messages(r):
        return {"messages": [{"role": "system", "content": r["system"]},
                             {"role": "user", "content": r["prompt"]},
                             {"role": "assistant", "content": r["answer"]}]}

    ds = Dataset.from_list([to_messages(r) for r in rows]).train_test_split(test_size=0.03, seed=609)
    model = AutoModelForCausalLM.from_pretrained(base, torch_dtype=torch.bfloat16, attn_implementation="sdpa")
    lora = LoraConfig(r=rank, lora_alpha=rank * 2, lora_dropout=0.05, bias="none", task_type="CAUSAL_LM",
                      target_modules="all-linear")
    cfg = SFTConfig(
        output_dir=out,
        num_train_epochs=epochs,
        per_device_train_batch_size=2,
        gradient_accumulation_steps=8,
        learning_rate=1e-4,
        lr_scheduler_type="cosine",
        warmup_ratio=0.03,
        bf16=True,
        logging_steps=10,
        eval_strategy="steps",
        eval_steps=100,
        save_strategy="epoch",
        max_length=8192,            # the daemon's context: a prompt and its answer fit
        assistant_only_loss=True,   # loss on the answer, not the prompt
        packing=False,
        report_to="none",
    )
    trainer = SFTTrainer(model=model, args=cfg, train_dataset=ds["train"], eval_dataset=ds["test"],
                         processing_class=tok, peft_config=lora)
    trainer.train()
    trainer.save_model(out)
    tok.save_pretrained(out)
    print(f"adapter in {out}; {len(rows)} pairs, {epochs} epochs, rank {rank}")


if __name__ == "__main__":
    if len(sys.argv) < 4:
        print(__doc__.strip(), file=sys.stderr)
        sys.exit(2)
    main(sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]) if len(sys.argv) > 4 else 2,
         int(sys.argv[5]) if len(sys.argv) > 5 else 32)
