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





# 创建 Dictionary 实例
my_dictionary = Dictionary('EnWords.csv')

# 循环查询
while True:
    print(" ")
    print("*" * 40)
    unknown_word = input("Enter a word to translate (or type 'exit' to quit): ")
    if unknown_word.lower() == 'exit':
        break
    result = my_dictionary.translate(unknown_word)
    print(result)
