# main.py
from search import Dictionary, ParagraphTranslator, YoudaoTranslator

if __name__ == "__main__":
    app_id = "25c4449fe9df2f01"
    app_key = "CbSO7HDBtQK3OtLBFJUP937NTiHnCseQ"
    translator = YoudaoTranslator(app_id, app_key)

    my_dictionary = Dictionary('EnWords.csv')
    my_paragraph = ParagraphTranslator()

    while True:
        print(" ")
        print("*" * 40)
        print("选择功能:")
        print("1. 查询单词")
        print("2. openai翻译")
        print("3. 有道翻译")
        print("4. 退出")

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
            translate_text = input("请输入要翻译的文本 (或者输入 'exit' 退出): ")
            if translate_text.lower() == 'exit':
                break
            from_lang = input("请输入源语言代码 (例如，'en' 表示英语): ")
            to_lang = input("请输入目标语言代码 (例如，'zh-CHS' 表示中文简体): ")
            translation = translator.translate(translate_text, from_lang, to_lang)
            print("需要翻译的文本：" + translate_text)
            print("翻译后的结果：" + translation)

        elif choice == '4':
            break

        else:
            print("无效的选择，请重新输入。")