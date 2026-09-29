TEST_F(ComplexTextControllerTest, EdenTextMeasurementCacheEncodingsAndZeroSuffixes) {
    TextMeasurementCache<unsigned, 0, 0, 0, 8192, 128> cache;
    std::array<Latin1Character, 4> narrow{'a', 'b', 0, 0};
    std::array<char16_t, 4> wide{'a', 'b', 0, 0};
    for (size_t length = 2; length <= narrow.size(); ++length) {
        auto *entry = cache.add(
            StringView(std::span<const Latin1Character>{narrow}.first(length)),
            static_cast<unsigned>(length)
        );
        ASSERT_NE(entry, nullptr);
        EXPECT_EQ(*entry, length);
    }
    for (size_t length = 2; length <= wide.size(); ++length) {
        auto *entry = cache.add(StringView(std::span<const char16_t>{wide}.first(length)), 999u);
        ASSERT_NE(entry, nullptr);
        EXPECT_EQ(*entry, length);
    }
    wide[1] = 0x0162;
    auto *nonLatin = cache.add(StringView(std::span<const char16_t>{wide}.first(2)), 42u);
    ASSERT_NE(nonLatin, nullptr);
    EXPECT_EQ(*nonLatin, 42u);
    auto *original = cache.add(StringView(std::span<const Latin1Character>{narrow}.first(2)), 999u);
    ASSERT_NE(original, nullptr);
    EXPECT_EQ(*original, 2u);
}

TEST_F(ComplexTextControllerTest, EdenTextMeasurementCacheLengthBoundaries) {
    TextMeasurementCache<unsigned, 0, 0, 0, 8192, 128> cache;
    std::array<char16_t, 129> characters;
    characters.fill('a');
    EXPECT_EQ(cache.add(StringView(std::span<const char16_t>{characters}.first(0)), 1u), nullptr);
    EXPECT_EQ(cache.add(StringView(std::span<const char16_t>{characters}), 1u), nullptr);
    for (auto length : {1u, 2u, 7u, 8u, 15u, 16u, 31u, 32u, 63u, 64u, 127u, 128u}) {
        auto text = StringView(std::span<const char16_t>{characters}.first(length));
        auto *entry = cache.add(text, unsigned{length});
        ASSERT_NE(entry, nullptr);
        EXPECT_EQ(*entry, length);
        entry = cache.add(text, 999u);
        ASSERT_NE(entry, nullptr);
        EXPECT_EQ(*entry, length);
    }
    characters[127] = 'b';
    auto *tail = cache.add(StringView(std::span<const char16_t>{characters}.first(128)), 512u);
    ASSERT_NE(tail, nullptr);
    EXPECT_EQ(*tail, 512u);
    characters[127] = 'a';
    auto *original = cache.add(StringView(std::span<const char16_t>{characters}.first(128)), 999u);
    ASSERT_NE(original, nullptr);
    EXPECT_EQ(*original, 128u);

    TextMeasurementCache<unsigned, 0, 0, 0, 8192, 256> largerCache;
    std::array<char16_t, 256> largerCharacters;
    largerCharacters.fill('a');
    ASSERT_NE(largerCache.add(StringView(std::span<const char16_t>{largerCharacters}), 1u), nullptr);
    largerCharacters.back() = 'b';
    auto *differentTail = largerCache.add(StringView(std::span<const char16_t>{largerCharacters}), 2u);
    ASSERT_NE(differentTail, nullptr);
    EXPECT_EQ(*differentTail, 2u);
    largerCharacters.back() = 'a';
    auto *originalTail = largerCache.add(StringView(std::span<const char16_t>{largerCharacters}), 999u);
    ASSERT_NE(originalTail, nullptr);
    EXPECT_EQ(*originalTail, 1u);
}

TEST_F(ComplexTextControllerTest, EdenTextMeasurementCacheRehashAndClear) {
    TextMeasurementCache<unsigned, 0, 0, 0, 8192, 128> cache;
    std::array<char16_t, 8> characters{};
    for (unsigned index = 0; index < 2048; ++index) {
        characters[0] = index;
        characters[7] = 0xd800 + (index & 0x7ff);
        auto *entry = cache.add(StringView(std::span<const char16_t>{characters}), unsigned{index});
        ASSERT_NE(entry, nullptr);
        EXPECT_EQ(*entry, index);
    }
    for (unsigned index = 0; index < 2048; ++index) {
        characters[0] = index;
        characters[7] = 0xd800 + (index & 0x7ff);
        auto *entry = cache.add(StringView(std::span<const char16_t>{characters}), 9999u);
        ASSERT_NE(entry, nullptr);
        EXPECT_EQ(*entry, index);
    }
    cache.clear();
    EXPECT_TRUE(cache.isEmpty());
    auto *entry = cache.add(StringView(std::span<const char16_t>{characters}), 777u);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(*entry, 777u);
}

TEST_F(ComplexTextControllerTest, EdenTextMeasurementCacheCapacityReset) {
    TextMeasurementCache<unsigned, 0, 0, 0, 4, 128> cache;
    std::array<char16_t, 2> characters{'a', 0};
    for (unsigned index = 0; index < 3; ++index) {
        characters[1] = index;
        ASSERT_NE(cache.add(StringView(std::span<const char16_t>{characters}), unsigned{index}), nullptr);
    }
    characters[1] = 3;
    EXPECT_EQ(cache.add(StringView(std::span<const char16_t>{characters}), 3u), nullptr);
    EXPECT_TRUE(cache.isEmpty());
    characters[1] = 0;
    auto *entry = cache.add(StringView(std::span<const char16_t>{characters}), 888u);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(*entry, 888u);
}
