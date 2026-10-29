import prismIncludeLanguages from "@theme-original/prism-include-languages";

// Prism has no Ren'Py grammar. The docs fence code as `renpy` so GitHub highlights it;
// here those blocks get Python's, which covers the `$` and `init python` lines.
export default function includeLanguages(Prism) {
  prismIncludeLanguages(Prism);
  Prism.languages.renpy = Prism.languages.python;
}
