import nltk
from gensim.models import Word2Vec
from nltk.corpus import stopwords

# 下载停用词列表（如果还未下载）
nltk.download("stopwords")

# 加载停用词
stop_words = set(stopwords.words("english"))

# 示例文本数据，可以根据需要替换成自己的文本数据
corpus = [
    "RED BANK, New Jersey (Achieve3000, September 9, 2020). Ready to snack on some spicy corn that fans of Mexican cuisine consider gastronomic gold? How about a heaping helping of aromatic Puerto Rican arroz con pollo (chicken with rice) or a tasty tostada from Guatemala? And if you've got room left for dessert, consider the chance to chomp a chocotorta cake from Argentina or a crunchy-sweet churro from Spain."
    "Plenty of Americans feast on these foods throughout the year, but Hispanic Heritage Month—which runs annually from September 15 through October 15—is an especially good chance to pile up your plate. That's because it's a time to salute Hispanic Americans, honor Hispanic history and traditions, and celebrate Hispanic cultural heritage—including its varied and flavorful victuals."
    "Starting Hispanic Heritage Month midway through September instead of at the beginning of the month might seem odd, but it actually makes a lot of sense. That's because September 15 is the date that five Latin American countries observe their Independence Day: Costa Rica, El Salvador, Guatemala, Honduras, and Nicaragua. Other Latin American countries mark their Independence Day on neighboring dates, with Mexico's falling on September 16 and Chile's on the 18th."
    "In fact, Hispanic Heritage Month honors all Americans who are from—or whose families are from—the many places across the globe where Spanish is the primary language. That includes most countries in South America and Central America and some in the Caribbean, as well as Mexico and Spain."
    "All in all, there are about 60 million Hispanic Americans in the United States. This mighty multitude is the nation's second largest ethnic group, representing about 20 percent of the total U.S. population. While Hispanic Americans hail from many parts of the world, Mexico, Puerto Rico, Cuba, El Salvador, and the Dominican Republic are—in terms of numbers—the top five areas of origin."
    "But no matter where their families are from, all denizens of the U.S. are invited to celebrate Hispanic Heritage Month. At parades, parties, and festivals across the country, revelers raise flags from around the world and honor the diverse cultures of Hispanic Americans. The sights and sounds at these events include colorful costumes, lively dancing, and lots of music—from traditional styles such as ranchera, cumbia, salsa, and merengue to modern genres like Latin hip-hop and reggaeton. And, of course, celebrants need sustenance to fuel all that fun, so the festivities usually feature a plenitude of delicious, authentic foods."
    "But Hispanic Heritage Month is about more than tasty treats and Latin beats. It's a time to remember Hispanic history and pay tribute to the generations of Hispanic Americans who have made crucial contributions to the United States. In every aspect of American life, from government and business to science, literature, and art, Hispanic leaders and pioneers have had an important, lasting impact—and their influence continues to grow. Museums, libraries, and schools mark the month with exhibits, workshops, performances, and other events that highlight the contributions and achievements of Hispanic Americans. Kids and teens can get involved, too. Every year, the National Council of Hispanic Employment Program Managers chooses a unique theme for Hispanic Heritage Month and invites submissions for short essays and posters that embody the theme."
    "However people mark the occasion, Hispanic Heritage Month is a chance for all Americans to celebrate Hispanic American culture—past, present, and future. Viva Hispanic Heritage Month!"

]

# 预处理文本数据
def preprocess(text):
    tokens = nltk.word_tokenize(text)
    tokens = [word.lower() for word in tokens if word.isalpha() and word.lower() not in stop_words]
    return tokens

# 对语料库中的文本数据进行预处理
preprocessed_corpus = [preprocess(text) for text in corpus]

# 训练Word2Vec模型
model = Word2Vec(preprocessed_corpus, vector_size=100, window=5, min_count=1, sg=0)

# 输入要查找相似词的词汇
input_word = ""

# 获取相似词
similar_words = model.wv.most_similar(input_word, topn=10)

# 显示相似词
print(f"Words similar to '{input_word}':")
for word, score in similar_words:
    print(f"{word}: {score}")
