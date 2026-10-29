// @ts-check
const {themes} = require("prism-react-renderer");

// Served at https://jrpg.com/stablecoinforgames/docs/. Every page and asset URL starts
// with baseUrl, so the host must serve the build under that same path.
const url = process.env.DOCS_URL || "https://jrpg.com";
const baseUrl = process.env.DOCS_BASE_URL || "/stablecoinforgames/docs/";

// Dracula on the jrpg dark surface instead of its own grey-blue.
const darkCode = {...themes.dracula, plain: {...themes.dracula.plain, backgroundColor: "#15101e"}};

/** @type {import('@docusaurus/types').Config} */
module.exports = {
  title: "Stablecoin for Games",
  tagline: "USDC and USDT tips and unlocks, paid straight to the developer's wallet",
  url,
  baseUrl,
  favicon: "img/favicon.svg",
  organizationName: "Repulse-Game-Studio",
  projectName: "jrpg-stablecoin-games",
  trailingSlash: false,
  onBrokenLinks: "throw",
  onBrokenAnchors: "throw",
  markdown: {
    format: "detect",
    hooks: {onBrokenMarkdownLinks: "throw"},
  },
  presets: [
    [
      "classic",
      /** @type {import('@docusaurus/preset-classic').Options} */
      ({
        docs: {
          path: "../docs",
          routeBasePath: "/",
          sidebarPath: "./sidebars.json",
          editUrl: "https://github.com/Repulse-Game-Studio/jrpg-stablecoin-games/edit/main/",
        },
        blog: false,
        pages: false,
        theme: {customCss: "./src/css/custom.css"},
      }),
    ],
  ],
  themeConfig: {
    image: "img/social-card.png",
    colorMode: {defaultMode: "dark", respectPrefersColorScheme: false},
    navbar: {
      title: "Stablecoin for Games",
      logo: {alt: "Stablecoin for Games", src: "img/logo.svg", width: 32, height: 32},
      items: [
        {to: "/players/paying", label: "Players", position: "left"},
        {to: "/developers", label: "Developers", position: "left"},
        {
          href: "https://github.com/Repulse-Game-Studio/jrpg-stablecoin-games",
          label: "GitHub",
          position: "right",
        },
      ],
    },
    footer: {
      links: [
        {
          title: "Docs",
          items: [
            {label: "For players", to: "/players/paying"},
            {label: "For developers", to: "/developers"},
            {label: "API reference", to: "/developers/use/api-reference"},
          ],
        },
        {
          title: "JRPG",
          items: [
            {label: "jrpg.com", href: "https://jrpg.com/"},
            {label: "Crypto games", href: "https://jrpg.com/games/"},
          ],
        },
        {
          title: "Code",
          items: [
            {label: "GitHub", href: "https://github.com/Repulse-Game-Studio/jrpg-stablecoin-games"},
            {label: "MIT license", href: "https://github.com/Repulse-Game-Studio/jrpg-stablecoin-games/blob/main/LICENSE"},
          ],
        },
      ],
      copyright: `© ${new Date().getFullYear()} Repulse Game Studio`,
    },
    prism: {
      theme: themes.github,
      darkTheme: darkCode,
      additionalLanguages: ["python"],
    },
  },
};
