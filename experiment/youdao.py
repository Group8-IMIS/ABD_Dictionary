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


if __name__ == "__main__":
    app_id = "25c4449fe9df2f01"
    app_key = "CbSO7HDBtQK3OtLBFJUP937NTiHnCseQ"
    translator = YoudaoTranslator(app_id, app_key)

    translate_text = "i can bit you!"
    from_lang = "en"
    to_lang = "zh-CHS"

    translation = translator.translate(translate_text, from_lang, to_lang)
    print("需要翻译的文本：" + translate_text)
    print("翻译后的结果：" + translation)
