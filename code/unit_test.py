import unittest
# main.py
from search import Dictionary, ParagraphTranslator, YoudaoTranslator

class TestDictionary(unittest.TestCase):
    def setUp(self):
        self.dictionary = Dictionary('EnWords.csv')  # Replace 'your_csv_file.csv' with the actual path

    def test_exact_translation(self):
        translation = self.dictionary.translate('apple')
        self.assertEqual(translation, "Word: apple\nTranslation: Translation of apple")  # Replace with the expected translation

    def test_case_insensitive_translation(self):
        translation = self.dictionary.translate('Apple')
        self.assertEqual(translation, "Word: Apple\nTranslation: Translation of apple")  # Replace with the expected translation

    def test_fuzzy_translation(self):
        # Assuming 'appel' is a similar word in the dictionary
        translation = self.dictionary.translate('appel')
        self.assertIn("Did you mean one of these?", translation)
        self.assertIn("apple", translation)

    def test_invalid_word(self):
        translation = self.dictionary.translate('xyz')
        self.assertEqual(translation, "xyz not found in the dictionary.")

if __name__ == '__main__':
    unittest.main()
