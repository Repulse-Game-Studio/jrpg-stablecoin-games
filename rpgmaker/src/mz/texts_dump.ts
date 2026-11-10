// Prints DEFAULT_TEXTS (texts.ts) as JSON, for tools/check_text_ids.py:
//   node src/mz/texts_dump.ts
import { DEFAULT_TEXTS } from "./texts.ts";

console.log(JSON.stringify(DEFAULT_TEXTS, null, 2));
