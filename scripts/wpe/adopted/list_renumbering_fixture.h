#pragma once

#include <wtf/text/WTFString.h>

namespace TestWebKitAPI {

inline WTF::String edenListRenumberingFixture()
{
    return WTF::String::fromUTF8("<ol id='descending' reversed><li id='reverse-one'>one</li><li id='reverse-two'>two</li><li id='reverse-three'>three</li></ol><ol id='explicit' start='7'><li id='explicit-one'>one</li><li id='explicit-value' value='12'>twelve</li><li id='explicit-three'>three</li></ol><ol id='outer' start='4'><li id='outer-one'>one<ol><li id='inner-one'>inner one</li><li id='inner-two'>inner two</li></ol></li><li id='outer-two'>two</li></ol><ol id='css' style='counter-reset: list-item 20'><li id='css-one'>one</li><li id='css-set' style='counter-set: list-item 7'>seven</li><li id='css-three'>three</li></ol>");
}

}
