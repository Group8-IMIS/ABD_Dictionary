import csv
import difflib


class Dictionary:
    def __init__(self, csv_file):
        self.dictionary = []
        self.load_dictionary(csv_file)

    def load_dictionary(self, csv_file):
        with open(csv_file, mode='r', encoding='utf-8') as file:
            reader = csv.DictReader(file)
            for row in reader:
                word = row['word']
                translation = row['translation']
                self.dictionary.append({'word': word.lower(), 'translation': translation})

        # 对字典按单词进行排序
        self.dictionary.sort(key=lambda x: x['word'])

    def binary_search(self, word):
        left, right = 0, len(self.dictionary) - 1
        while left <= right:
            mid = (left + right) // 2
            mid_word = self.dictionary[mid]['word']
            if mid_word == word:
                return self.dictionary[mid]['translation']
            elif mid_word < word:
                left = mid + 1
            else:
                right = mid - 1
        return None

    def translate(self, unknown_word):
        word_to_lookup = unknown_word.lower()  # 转换为小写

        translation = self.binary_search(word_to_lookup)

        if translation:
            return f"Word: {unknown_word}\nTranslation: {translation}"
        else:
            # 如果没有找到精确匹配，尝试模糊匹配
            close_matches = difflib.get_close_matches(word_to_lookup, [entry['word'] for entry in self.dictionary], n=5,
                                                      cutoff=0.6)
            if close_matches:
                suggestions = "\nDid you mean one of these?\n" + "\n".join(close_matches)
                user_choice = input(
                    f"{unknown_word} not found. {suggestions}\nPlease select a suggestion or enter another word: ")
                if user_choice in close_matches:
                    selected_word = user_choice
                    selected_translation = self.binary_search(selected_word)
                    return f"Word: {selected_word}\nTranslation: {selected_translation}"
                else:
                    return f"Invalid choice. {unknown_word} not found in the dictionary."
            else:
                return f"{unknown_word} not found in the dictionary."




import openai

# 设置你的OpenAI API密钥
api_key = "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"
openai.api_key = api_key

# 创建ParagraphTranslator类的实例
class ParagraphTranslator:
    def translate_paragraph(self, text, target_language):
        try:
            # 调用GPT-3来进行翻译
            response = openai.Completion.create(
                engine="text-davinci-003",
                prompt=f"Translate the following text to {target_language}: {text}",
                max_tokens=1  # 根据需要调整生成的最大令牌数
            )

            # 提取翻译后的文本
            translated_text = response.choices[0].text.strip()

            return translated_text

        except Exception as e:
            print(f"发生了错误: {e}")
            return None





import requests
import time
import hashlib
import uuid

class YoudaoTranslator:
    def __init__(self, app_id, app_key):
        self.youdao_url = 'https://openapi.youdao.com/api'
        self.app_id = app_id
        self.app_key = app_key

    def generate_sign(self, translate_text):
        input_text = ""
        if len(translate_text) <= 20:
            input_text = translate_text
        elif len(translate_text) > 20:
            input_text = translate_text[:10] + str(len(translate_text)) + translate_text[-10:]

        time_curtime = int(time.time())
        uu_id = uuid.uuid4()
        sign = hashlib.sha256(
            (self.app_id + input_text + str(uu_id) + str(time_curtime) + self.app_key).encode('utf-8')).hexdigest()
        return sign, time_curtime, uu_id

    def translate(self, translate_text, from_lang, to_lang):
        sign, time_curtime, uu_id = self.generate_sign(translate_text)

        data = {
            'q': translate_text,
            'from': from_lang,
            'to': to_lang,
            'appKey': self.app_id,
            'salt': uu_id,
            'sign': sign,
            'signType': "v3",
            'curtime': time_curtime,
        }

        response = requests.get(self.youdao_url, params=data).json()
        translation = response["translation"][0] if "translation" in response else "Translation not available"
        return translation



