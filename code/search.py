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

my_dictionary = Dictionary('EnWords.csv')
my_paragraph = ParagraphTranslator()

while True:
    print(" ")
    print("*" * 40)
    print("选择功能:")
    print("1. 查询单词")
    print("2. 翻译段落")
    print("3. 退出")

    choice = input("请输入数字选择功能: ")

    if choice == '1':
        unknown_word = input("请输入要查询的单词 (或者输入 'exit' 退出): ")
        if unknown_word.lower() == 'exit':
            break
        result = my_dictionary.translate(unknown_word)
        print(result)

    elif choice == '2':
        paragraph = input("请输入要翻译的段落 (或者输入 'exit' 退出): ")
        if paragraph.lower() == 'exit':
            break
        target_language = input("请输入目标语言代码 (例如，'en' 表示英语): ")
        translated_paragraph = my_paragraph.translate_paragraph(paragraph, target_language)
        if translated_paragraph:
            print(f"翻译结果: {translated_paragraph}")

    elif choice == '3':
        break

    else:
        print("无效的选择，请重新输入。")
