import os
import math
import argparse

import torch
from torch.utils.data import DataLoader

from tqdm import tqdm

from transformers import (
    PreTrainedTokenizerBase,
    DataCollatorForSeq2Seq,
)

from model import load_model_for_inference

from dataset import DatasetReader, count_lines

from accelerate import Accelerator, DistributedType, find_executable_batch_size


def encode_string(text):
    return text.replace("\r", r"\r").replace("\n", r"\n").replace("\t", r"\t")


def get_dataloader(
    accelerator: Accelerator,
    filename: str,
    tokenizer: PreTrainedTokenizerBase,
    batch_size: int,
    max_length: int,
    prompt: str,
) -> DataLoader:
    dataset = DatasetReader(filename, tokenizer, max_length, prompt)
    if accelerator.distributed_type == DistributedType.TPU:
        data_collator = DataCollatorForSeq2Seq(
            tokenizer,
            padding="max_length",
            max_length=max_length,
            label_pad_token_id=tokenizer.pad_token_id,
            return_tensors="pt",
        )
    else:
        data_collator = DataCollatorForSeq2Seq(
            tokenizer,
            padding=True,
            label_pad_token_id=tokenizer.pad_token_id,
            # max_length=max_length, No need to set max_length here, we already truncate in the preprocess function
            pad_to_multiple_of=8,
            return_tensors="pt",
        )

    return DataLoader(
        dataset,
        batch_size=batch_size,
        collate_fn=data_collator,
        num_workers=0,  # Disable multiprocessing
    )


def main(
    sentences_path: str = "sample_text/en1.txt",
    output_path: str = "sample_text/en1_zh.txt",
    source_lang: str = "en",
    target_lang: str = "zh",
    starting_batch_size: int = 128,
    model_name: str = "facebook/m2m100_1.2B",
    lora_weights_name_or_path: str = None,
    force_auto_device_map: bool = False,
    precision: str = None,
    max_length: int = 128,
    num_beams: int = 4,
    num_return_sequences: int = 1,
    do_sample: bool = False,
    temperature: float = 1.0,
    top_k: int = 50,
    top_p: float = 1.0,
    keep_special_tokens: bool = False,
    keep_tokenization_spaces: bool = False,
    repetition_penalty: float = None,
    prompt: str = None,
):
    os.makedirs(os.path.abspath(os.path.dirname(output_path)), exist_ok=True)

    accelerator = Accelerator()

    if force_auto_device_map and starting_batch_size >= 64:
        print(
            f"WARNING: You are using a very large batch size ({starting_batch_size}) and the auto_device_map  flag. "
            f"auto_device_map will offload model parameters to the CPU when they don't fit on the GPU VRAM. "
            f"If you use a very large batch size, it will offload a lot of parameters to the CPU and slow down the "
            f"inference. You should consider using a smaller batch size, i.e '--starting_batch_size 8'"
        )

    if precision is None:
        quantization = None
        dtype = None
    elif precision == "8" or precision == "4":
        quantization = int(precision)
        dtype = None
    elif precision == "fp16":
        quantization = None
        dtype = "float16"
    elif precision == "bf16":
        quantization = None
        dtype = "bfloat16"
    elif precision == "32":
        quantization = None
        dtype = "float32"
    else:
        raise ValueError(
            f"Precision {precision} not supported. Please choose between 8, 4, fp16, bf16, 32 or None."
        )

    model, tokenizer = load_model_for_inference(
        weights_path=model_name,
        quantization=quantization,
        lora_weights_name_or_path=lora_weights_name_or_path,
        torch_dtype=dtype,
        force_auto_device_map=force_auto_device_map,
    )

    is_translation_model = hasattr(tokenizer, "lang_code_to_id")

    if is_translation_model and (source_lang is None or target_lang is None):
        raise ValueError(
            f"The model you are using requires a source and target language. "
            f"Please specify them with --source-lang and --target-lang. "
            f"The supported languages are: {tokenizer.lang_code_to_id.keys()}"
        )
    if not is_translation_model and (
        source_lang is not None or target_lang is not None
    ):
        if prompt is None:
            print(
                "WARNING: You are using a model that does not support source and target languages parameters "
                "but you specified them. You probably want to use m2m100/nllb200 for translation or "
                "set --prompt to define the task for you model. "
            )
        else:
            print(
                "WARNING: You are using a model that does not support source and target languages parameters "
                "but you specified them."
            )

    if prompt is not None and "%%SENTENCE%%" not in prompt:
        raise ValueError(
            f"The prompt must contain the %%SENTENCE%% token to indicate where the sentence should be inserted. "
            f"Your prompt: {prompt}"
        )

    if is_translation_model:
        try:
            _ = tokenizer.lang_code_to_id[source_lang]
        except KeyError:
            raise KeyError(
                f"Language {source_lang} not found in tokenizer. Available languages: {tokenizer.lang_code_to_id.keys()}"
            )
        tokenizer.src_lang = source_lang

        try:
            lang_code_to_idx = tokenizer.lang_code_to_id[target_lang]
        except KeyError:
            raise KeyError(
                f"Language {target_lang} not found in tokenizer. Available languages: {tokenizer.lang_code_to_id.keys()}"
            )
        if "small100" in model_name:
            tokenizer.tgt_lang = target_lang
            # We don't need to force the BOS token, so we set is_translation_model to False
            is_translation_model = False

    gen_kwargs = {
        "max_length": max_length,
        "num_beams": num_beams,
        "num_return_sequences": num_return_sequences,
        "do_sample": do_sample,
        "temperature": temperature,
        "top_k": top_k,
        "top_p": top_p,
    }

    if repetition_penalty is not None:
        gen_kwargs["repetition_penalty"] = repetition_penalty

    total_lines: int = count_lines(sentences_path)

    if accelerator.is_main_process:
        print(
            f"** Translation **\n"
            f"Input file: {sentences_path}\n"
            f"Output file: {output_path}\n"
            f"Source language: {source_lang}\n"
            f"Target language: {target_lang}\n"
            f"Force target lang as BOS token: {is_translation_model}\n"
            f"Prompt: {prompt}\n"
            f"Starting batch size: {starting_batch_size}\n"
            f"Device: {str(accelerator.device).split(':')[0]}\n"
            f"Num. Devices: {accelerator.num_processes}\n"
            f"Distributed_type: {accelerator.distributed_type}\n"
            f"Max length: {max_length}\n"
            f"Quantization: {quantization}\n"
            f"Precision: {dtype}\n"
            f"Model: {model_name}\n"
            f"LoRA weights: {lora_weights_name_or_path}\n"
            f"Force auto device map: {force_auto_device_map}\n"
            f"Keep special tokens: {keep_special_tokens}\n"
            f"Keep tokenization spaces: {keep_tokenization_spaces}\n"
        )
        print("** Generation parameters **")
        print("\n".join(f"{k}: {v}" for k, v in gen_kwargs.items()))
        print("\n")

    @find_executable_batch_size(starting_batch_size=starting_batch_size)
    def inference(batch_size):
        nonlocal model, tokenizer, sentences_path, max_length, output_path, lang_code_to_idx, gen_kwargs, precision, prompt, is_translation_model

        print(f"Translating with batch size {batch_size}")

        data_loader = get_dataloader(
            accelerator=accelerator,
            filename=sentences_path,
            tokenizer=tokenizer,
            batch_size=batch_size,
            max_length=max_length,
            prompt=prompt,
        )

        model, data_loader = accelerator.prepare(model, data_loader)

        samples_seen: int = 0

        with tqdm(
            total=total_lines,
            desc="Dataset translation",
            leave=True,
            ascii=True,
            disable=(not accelerator.is_main_process),
        ) as pbar, open(output_path, "w", encoding="utf-8") as output_file:
            with torch.no_grad():
                for step, batch in enumerate(data_loader):
                    batch["input_ids"] = batch["input_ids"]
                    batch["attention_mask"] = batch["attention_mask"]

                    generated_tokens = accelerator.unwrap_model(model).generate(
                        **batch,
                        forced_bos_token_id=lang_code_to_idx
                        if is_translation_model
                        else None,
                        **gen_kwargs,
                    )

                    generated_tokens = accelerator.pad_across_processes(
                        generated_tokens, dim=1, pad_index=tokenizer.pad_token_id
                    )

                    generated_tokens = (
                        accelerator.gather(generated_tokens).cpu().numpy()
                    )

                    tgt_text = tokenizer.batch_decode(
                        generated_tokens,
                        skip_special_tokens=not keep_special_tokens,
                        clean_up_tokenization_spaces=not keep_tokenization_spaces,
                    )
                    if accelerator.is_main_process:
                        if (
                            step
                            == math.ceil(
                                math.ceil(total_lines / batch_size)
                                / accelerator.num_processes
                            )
                            - 1
                        ):
                            tgt_text = tgt_text[
                                : (total_lines * num_return_sequences) - samples_seen
                            ]
                        else:
                            samples_seen += len(tgt_text)

                        print(
                            "\n".join(
                                [encode_string(sentence) for sentence in tgt_text]
                            ),
                            file=output_file,
                        )

                    pbar.update(len(tgt_text) // gen_kwargs["num_return_sequences"])

    inference()
    print(f"Translation done.\n")


if __name__ == "__main__":
    main(
        sentences_path="sample_text/en1.txt",
        output_path="sample_text/en2es.translation.m2m100_1.2B123.txt",
        source_lang="en",
        target_lang="zh",
        starting_batch_size=128,
        model_name="facebook/m2m100_1.2B",
        max_length=128,
        num_beams=5,
        num_return_sequences=1,
        precision=None,
        do_sample=False,
        temperature=0.8,
        top_k=100,
        top_p=0.75,
        keep_special_tokens=False,
        keep_tokenization_spaces=False,
        repetition_penalty=None,
        prompt=None,
    )
