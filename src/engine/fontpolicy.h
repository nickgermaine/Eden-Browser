#pragma once

#include <QString>

namespace eden::engine {

    enum class FontHinting { None, Slight, Medium, Full };

    class FontPolicy {
      public:
        static QString systemFamily();
        static QString systemDescription();
        static QString standardFamily();
        static QString sansSerifFamily();
        static QString serifFamily();
        static QString monospaceFamily();
        static QString cursiveFamily();
        static QString fantasyFamily();
        static bool antialiasing();
        static bool subpixelAntialiasing();
        static FontHinting hinting();
        static QString resolveGenericFamily(const QString &generic);
        static QString familyFromDescription(const QString &description);
        static void resetCache();
    };

}
