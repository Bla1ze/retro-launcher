#include "Theme.h"

// The active look's tokens, copied from its namespace by setStyle(). Neon until
// then, so anything drawn before settings are read looks as it always has.
namespace Theme {

namespace {
Style g_style = Style::Neon;
}

SDL_Color Ink = neon::Ink, BgTop = neon::BgTop, BgBottom = neon::BgBottom, HeaderL = neon::HeaderL,
          HeaderR = neon::HeaderR, Accent = neon::Accent, Accent2 = neon::Accent2, Gold = neon::Gold,
          Rose = neon::Rose, Live = neon::Live, Text = neon::Text, TextDim = neon::TextDim, Muted = neon::Muted,
          Faint = neon::Faint, Card = neon::Card, CardHi = neon::CardHi, Hairline = neon::Hairline;
namespace Type {
float Hero = neon::Type::Hero, Title = neon::Type::Title, Heading = neon::Type::Heading, Body = neon::Type::Body,
      Small = neon::Type::Small, Caption = neon::Type::Caption;
}

Style style() { return g_style; }

#define THEME_TOKENS(NS)                                                                                     \
    Ink = NS::Ink; BgTop = NS::BgTop; BgBottom = NS::BgBottom; HeaderL = NS::HeaderL; HeaderR = NS::HeaderR; \
    Accent = NS::Accent; Accent2 = NS::Accent2; Gold = NS::Gold; Rose = NS::Rose; Live = NS::Live;           \
    Text = NS::Text; TextDim = NS::TextDim; Muted = NS::Muted; Faint = NS::Faint; Card = NS::Card;           \
    CardHi = NS::CardHi; Hairline = NS::Hairline;                                                            \
    Type::Hero = NS::Type::Hero; Type::Title = NS::Type::Title; Type::Heading = NS::Type::Heading;           \
    Type::Body = NS::Type::Body; Type::Small = NS::Type::Small; Type::Caption = NS::Type::Caption;

void setStyle(Style s) {
    g_style = s;
    if (s == Style::Attract) { THEME_TOKENS(attract) }
    else { THEME_TOKENS(neon) }
}

#undef THEME_TOKENS

Style styleNamed(const std::string& name) { return name == "attract" ? Style::Attract : Style::Neon; }

const char* styleName(Style s) { return s == Style::Attract ? "attract" : "neon"; }

} // namespace Theme
