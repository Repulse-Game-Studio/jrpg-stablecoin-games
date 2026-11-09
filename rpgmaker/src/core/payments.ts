/**
 * Validation of a pay() request, independent of chain and network (twin of scfg_core/payments.py).
 *
 * Used synchronously by scfg.pay() (so programmer errors raise immediately in the developer's
 * script) and again by Engine.startSession.
 */
import { parseAmount } from "./amounts.ts";
import { chainsForToken, type Config } from "./config.ts";
import { Dec } from "./decimal.ts";
import { ConfigError } from "./errors.ts";
import { hasOwn, pyTruthy } from "./pystr.ts";

const dec = (s: string): Dec => Dec.parse(s) as Dec;

export const MIN_TIP: Dec = dec("0.50");
export const TIP_PRESETS: readonly Dec[] = [dec("1.00"), dec("3.00"), dec("5.00"), dec("10.00")];

export interface PaymentSpec {
  kind: string; // "unlock" | "tip"
  item: string | null;
  price: Dec;
  token: string;
  code: number; // 0 for tips
  title: string;
  chains: string[]; // enabled chains that carry the token, in display order
}

const none = (v: unknown): boolean => v === null || v === undefined;

export function preparePayment(
  config: Config,
  kind: unknown,
  opts: { item?: unknown; amount?: unknown; token?: unknown; title?: unknown } = {},
): PaymentSpec {
  for (const k of Object.keys(opts)) {
    if (!["item", "amount", "token", "title"].includes(k)) {
      throw new TypeError(`prepare_payment() got an unexpected keyword argument '${k}'`);
    }
  }
  const item = none(opts.item) ? null : opts.item;
  const amount = opts.amount;
  let token = none(opts.token) ? null : opts.token;
  let title = none(opts.title) ? null : opts.title;
  let price: Dec;
  let code: number;
  if (kind === "unlock") {
    if (typeof item === "object" && item !== null) throw new TypeError("unhashable item");
    if (typeof item !== "string" || !hasOwn(config.items, item)) {
      throw new ConfigError(`unknown item ${JSON.stringify(item)}; declare it with scfg.add_item()`, { code: "item.unknown" });
    }
    const it = config.items[item]!;
    price = none(amount) ? it.price : parseAmount(amount);
    if (price.cmp(it.min_price) < 0) {
      throw new ConfigError(`amount ${price} is below the item's min_price ${it.min_price}`, { code: "payment.below_min_price" });
    }
    token = pyTruthy(token) ? token : it.token;
    code = it.code;
    title = pyTruthy(title) ? title : it.title;
  } else if (kind === "tip") {
    if (none(amount) || token === null) {
      throw new ConfigError("tips need an amount and a token", { code: "tip.needs_amount_and_token" });
    }
    price = parseAmount(amount);
    if (price.cmp(MIN_TIP) < 0) {
      throw new ConfigError(`tips must be at least ${MIN_TIP}`, { code: "tip.below_minimum" });
    }
    code = 0;
    title = pyTruthy(title) ? title : "Tip";
  } else {
    throw new ConfigError(`kind must be 'unlock' or 'tip', not ${JSON.stringify(kind)}`, { code: "payment.bad_kind" });
  }
  if (typeof token !== "string" || !config.tokens.includes(token)) {
    throw new ConfigError(`token ${JSON.stringify(token)} is not enabled`, { code: "token.not_enabled" });
  }
  const chains = chainsForToken(config, token);
  if (chains.length === 0) {
    throw new ConfigError(`${token} is not available on any enabled chain`, { code: "token.unavailable" });
  }
  return { kind, item: item as string | null, price, token, code, title: title as string, chains };
}
