/** In-memory EVM chain for watcher/engine tests (twin of tests/unit/fakes.py FakeChain). Not a test file. */
import type { Chain } from "../src/core/chains.ts";
import { TRANSFER_TOPIC, addressTopic } from "../src/core/evm.ts";
import { RpcUnavailable } from "../src/core/rpc.ts";

const hex64 = (n: number | bigint): string => n.toString(16).padStart(64, "0");

/**
 * Same method names as RpcClient, driven by tests:
 * head, hashes (block -> hash; unlisted blocks get a default), logs (raw dicts returned by
 * getLogs, filtered by range and address), receipts (tx -> receipt or null), down (every
 * method raises RpcUnavailable). `logRange` is unset unless a test sets it.
 */
export class FakeChain {
  head: number;
  hashes = new Map<number, string>();
  logs: Record<string, any>[] = [];
  receipts: Record<string, any> = {};
  down = false;
  calls: string[] = [];
  lastGetLogs: [string, unknown[], number, number] | null = null;
  logRange?: number;
  chain_obj?: Chain;

  constructor(head = 1000) {
    this.head = head;
  }

  private check(name: string): void {
    this.calls.push(name);
    if (this.down) throw new RpcUnavailable("fake outage");
  }

  blockHash(n: number): string {
    return this.hashes.get(n) ?? "0x" + hex64(n);
  }

  async blockNumber(): Promise<number> {
    this.check("block_number");
    return this.head;
  }

  async getBlockHash(n: number): Promise<string | null> {
    this.check("get_block_hash");
    return n <= this.head ? this.blockHash(n) : null;
  }

  async getTransactionReceipt(tx: string): Promise<any> {
    this.check("get_transaction_receipt");
    return Object.prototype.hasOwnProperty.call(this.receipts, tx) ? this.receipts[tx] : null;
  }

  async getLogs(address: string, topics: unknown[], from: number, to: number): Promise<any[]> {
    this.check("get_logs");
    this.lastGetLogs = [address, topics, from, to];
    return this.logs.filter((lg) => {
      const b = parseInt(lg["blockNumber"], 16);
      return from <= b && b <= to && String(lg["address"]).toLowerCase() === address.toLowerCase();
    });
  }
}

export function transferLog(
  token: string, sender: string, recipient: string, value: number | bigint,
  opts: { block: number; tx_hash: string; log_index?: number; block_hash?: string },
): Record<string, any> {
  return {
    address: token.toLowerCase(),
    topics: [TRANSFER_TOPIC, addressTopic(sender), addressTopic(recipient)],
    data: "0x" + hex64(BigInt(value)),
    transactionHash: opts.tx_hash,
    logIndex: "0x" + (opts.log_index ?? 0).toString(16),
    blockNumber: "0x" + opts.block.toString(16),
    blockHash: opts.block_hash ?? "0x" + hex64(opts.block),
  };
}
