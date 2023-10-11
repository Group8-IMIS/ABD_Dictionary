from google.cloud import translate_v2 as translate
from google.cloud import exceptions

# 替换为你自己的Google Cloud API密钥
api_key = 'YOUR_API_KEY_HERE'

def translate_text(text, target_languages=['en'], source_language=None):
    try:
        client = translate.Client(api_key)
        translations = []

        # 自动检测源语言
        if source_language is None:
            source_language = client.detect_language(text)

        for target_language in target_languages:
            result = client.translate(text, target_language=target_language, source_language=source_language)
            translated_text = result['input']
            translated_text = result['translatedText']
            translations.append((target_language, translated_text))

        return translations
    except exceptions.GoogleAuthError as e:
        print(f"Google认证错误: {e}")
    except exceptions.GoogleAPIError as e:
        print(f"Google API错误: {e}")
    except Exception as e:
        print(f"发生未知错误: {e}")

if __name__ == '__main__':
    text_to_translate = "你好，世界！"  # 要翻译的文本
    target_languages = ['en', 'es', 'fr']  # 多个目标语言，可以根据需求添加或修改

    translations = translate_text(text_to_translate, target_languages)
    print(f"原文: {text_to_translate}")
    for target_language, translated_text in translations:
        print(f"目标语言 ({target_language}): {translated_text}")
