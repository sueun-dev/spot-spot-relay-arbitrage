#!/usr/bin/env python3
import base64
import hashlib
import hmac
import json
import os
import time
import urllib.parse
import urllib.request
import uuid
from datetime import datetime, timezone
from urllib.error import HTTPError, URLError

TARGET_USDT = 35.0
MIN_NET_KRW = 300.0
KOREAN_FEE_EVENTS = 1
FOREIGN_FEE_EVENTS = 3
FEE_RATES = {
    "Bithumb": 0.0004,
    "Upbit": 0.0005,
    "Bybit": 0.0010,
    "OKX": 0.0010,
}
PREFERRED_SYMBOLS = ["ADA", "SOL", "DOGE", "XRP", "TRX"]


def http_json(url, headers=None, timeout=20):
    req = urllib.request.Request(url)
    req.add_header(
        "User-Agent",
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
        "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/137.0.0.0 Safari/537.36",
    )
    for key, value in (headers or {}).items():
        req.add_header(key, value)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode())


def safe_http_json(url, headers=None, timeout=20):
    try:
        return http_json(url, headers=headers, timeout=timeout), None
    except HTTPError as exc:
        try:
            body = exc.read().decode()
        except Exception:
            body = ""
        return None, f"HTTP {exc.code}: {body}"
    except URLError as exc:
        return None, str(exc)


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def upbit_jwt(query=""):
    access_key = os.environ["UPBIT_API_KEY"]
    secret_key = os.environ["UPBIT_SECRET_KEY"]
    payload = {"access_key": access_key, "nonce": str(uuid.uuid4())}
    if query:
        payload["query_hash"] = hashlib.sha512(query.encode()).hexdigest()
        payload["query_hash_alg"] = "SHA512"
    header = b64url(json.dumps({"alg": "HS256", "typ": "JWT"}).encode())
    body = b64url(json.dumps(payload, separators=(",", ":")).encode())
    sig = b64url(hmac.new(secret_key.encode(), f"{header}.{body}".encode(), hashlib.sha256).digest())
    return f"{header}.{body}.{sig}"


def bithumb_jwt(query=""):
    access_key = os.environ["BITHUMB_API_KEY"]
    secret_key = os.environ["BITHUMB_SECRET_KEY"]
    payload = {
        "access_key": access_key,
        "nonce": str(uuid.uuid4()),
        "timestamp": int(time.time() * 1000),
    }
    if query:
        payload["query_hash"] = hashlib.sha512(query.encode()).hexdigest()
        payload["query_hash_alg"] = "SHA512"
    header = b64url(json.dumps({"alg": "HS256", "typ": "JWT"}).encode())
    body = b64url(json.dumps(payload, separators=(",", ":")).encode())
    sig = b64url(hmac.new(secret_key.encode(), f"{header}.{body}".encode(), hashlib.sha256).digest())
    return f"{header}.{body}.{sig}"


def bybit_headers(params=""):
    api_key = os.environ["BYBIT_API_KEY"]
    secret_key = os.environ["BYBIT_SECRET_KEY"]
    timestamp = str(int(time.time() * 1000))
    recv_window = "5000"
    sign = hmac.new(
        secret_key.encode(),
        (timestamp + api_key + recv_window + params).encode(),
        hashlib.sha256,
    ).hexdigest()
    return {
        "X-BAPI-API-KEY": api_key,
        "X-BAPI-SIGN": sign,
        "X-BAPI-TIMESTAMP": timestamp,
        "X-BAPI-RECV-WINDOW": recv_window,
        "Content-Type": "application/json",
    }


def okx_headers(method, path, body=""):
    api_key = os.environ["OKX_API_KEY"]
    secret_key = os.environ["OKX_SECRET_KEY"]
    passphrase = os.environ["OKX_PASSPHRASE"]
    timestamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"
    message = timestamp + method + path + body
    sign = base64.b64encode(hmac.new(secret_key.encode(), message.encode(), hashlib.sha256).digest()).decode()
    return {
        "OK-ACCESS-KEY": api_key,
        "OK-ACCESS-SIGN": sign,
        "OK-ACCESS-TIMESTAMP": timestamp,
        "OK-ACCESS-PASSPHRASE": passphrase,
        "Content-Type": "application/json",
    }


def normalize_chain(raw: str) -> str:
    normalized = "".join(ch.upper() for ch in raw if ch.isalnum())
    aliases = {
        "BITCOIN": "BTC",
        "ETHEREUM": "ETH",
        "POLYGON": "MATIC",
        "POL": "MATIC",
        "SOLANA": "SOL",
        "TRON": "TRX",
        "RIPPLE": "XRP",
        "LITECOIN": "LTC",
        "DOGECOIN": "DOGE",
        "AVALANCHE": "AVAXC",
        "AVALANCHECCHAIN": "AVAXC",
        "CARDANO": "ADA",
        "BSC": "BEP20",
        "BNBSMARTCHAIN": "BEP20",
        "OPTIMISM": "OP",
    }
    return aliases.get(normalized, normalized)


def choose_symbol(candidates):
    for symbol in PREFERRED_SYMBOLS:
        if symbol in candidates:
            return symbol
    return sorted(candidates)[0]


def relay_metrics(korean_ask, korean_ask_qty, foreign_bid, foreign_bid_qty, usdt_rate, korean_fee, foreign_fee, withdraw_fee):
    match_qty = min(korean_ask_qty, foreign_bid_qty)
    target_coin_qty = TARGET_USDT / foreign_bid
    effective_qty = min(match_qty, target_coin_qty)
    buy_krw = korean_ask * effective_qty
    sell_usdt = foreign_bid * effective_qty
    sell_krw = sell_usdt * usdt_rate
    korean_fee_krw = buy_krw * korean_fee * KOREAN_FEE_EVENTS
    foreign_fee_krw = sell_krw * foreign_fee * FOREIGN_FEE_EVENTS
    withdraw_fee_krw = withdraw_fee * korean_ask
    total_fee_krw = korean_fee_krw + foreign_fee_krw + withdraw_fee_krw
    net_profit_krw = (sell_krw - buy_krw) - total_fee_krw
    net_basis_krw = buy_krw + total_fee_krw
    return {
        "match_qty": match_qty,
        "target_coin_qty": target_coin_qty,
        "both_can_fill_target": (korean_ask * korean_ask_qty / usdt_rate) >= TARGET_USDT and (foreign_bid * foreign_bid_qty) >= TARGET_USDT,
        "net_profit_krw": net_profit_krw,
        "net_edge_pct": (net_profit_krw / net_basis_krw) * 100 if net_basis_krw > 0 else 0.0,
    }


def get_bithumb_quote(symbol):
    payload = http_json(f"https://api.bithumb.com/public/orderbook/{symbol}_KRW?count=1")
    data = payload["data"]
    return {
        "bid": float(data["bids"][0]["price"]),
        "bid_qty": float(data["bids"][0]["quantity"]),
        "ask": float(data["asks"][0]["price"]),
        "ask_qty": float(data["asks"][0]["quantity"]),
    }


def get_upbit_quote(symbol):
    payload = http_json(f"https://api.upbit.com/v1/orderbook?markets=KRW-{symbol}")
    level = payload[0]["orderbook_units"][0]
    return {
        "bid": float(level["bid_price"]),
        "bid_qty": float(level["bid_size"]),
        "ask": float(level["ask_price"]),
        "ask_qty": float(level["ask_size"]),
    }


def get_bybit_quote(symbol):
    payload = http_json(f"https://api.bybit.com/v5/market/orderbook?category=spot&symbol={symbol}USDT&limit=1")
    return {
        "bid": float(payload["result"]["b"][0][0]),
        "bid_qty": float(payload["result"]["b"][0][1]),
        "ask": float(payload["result"]["a"][0][0]),
        "ask_qty": float(payload["result"]["a"][0][1]),
    }


def get_okx_quote(symbol):
    payload = http_json(f"https://www.okx.com/api/v5/market/books?instId={symbol}-USDT&sz=1")
    book = payload["data"][0]
    return {
        "bid": float(book["bids"][0][0]),
        "bid_qty": float(book["bids"][0][1]),
        "ask": float(book["asks"][0][0]),
        "ask_qty": float(book["asks"][0][1]),
    }


def main():
    ip_info = http_json("http://ipinfo.io/json")

    bithumb_balances = http_json(
        "https://api.bithumb.com/v1/accounts",
        {"Authorization": "Bearer " + bithumb_jwt(), "accept": "application/json"},
    )
    upbit_balances = http_json(
        "https://api.upbit.com/v1/accounts",
        {"Authorization": "Bearer " + upbit_jwt(), "accept": "application/json"},
    )
    bybit_balance = http_json(
        "https://api.bybit.com/v5/account/wallet-balance?accountType=UNIFIED",
        bybit_headers("accountType=UNIFIED"),
    )
    okx_balance, okx_balance_error = safe_http_json(
        "https://www.okx.com/api/v5/account/balance",
        okx_headers("GET", "/api/v5/account/balance"),
    )

    bithumb_all = http_json("https://api.bithumb.com/public/ticker/ALL_KRW")
    bithumb_symbols = {base for base in bithumb_all["data"].keys() if base != "date"}
    upbit_markets = http_json("https://api.upbit.com/v1/market/all?isDetails=false")
    upbit_symbols = {row["market"].split("-")[1] for row in upbit_markets if row["market"].startswith("KRW-")}
    bybit_instruments = http_json("https://api.bybit.com/v5/market/instruments-info?category=spot&limit=1000")
    bybit_symbols = {row["baseCoin"] for row in bybit_instruments["result"]["list"] if row.get("quoteCoin") == "USDT" and row.get("status") == "Trading"}
    okx_instruments = http_json("https://www.okx.com/api/v5/public/instruments?instType=SPOT")
    okx_symbols = {row["baseCcy"] for row in okx_instruments["data"] if row.get("quoteCcy") == "USDT" and row.get("state") == "live"}

    common_union = (bithumb_symbols | upbit_symbols) & (bybit_symbols | okx_symbols)
    four_way_common = bithumb_symbols & upbit_symbols & bybit_symbols & okx_symbols
    if not common_union:
        raise RuntimeError("no common tradable symbols found")
    symbol = choose_symbol(four_way_common or common_union)

    bithumb_fees_raw = http_json("https://api.bithumb.com/v2/fee/inout/ALL")
    bithumb_statuses = http_json("https://api.bithumb.com/public/assetsstatus/ALL")
    bithumb_fee_networks = {}
    for row in bithumb_fees_raw:
        if row.get("currency") != symbol:
            continue
        for network in row.get("networks", []):
            net_name = normalize_chain(network.get("net_type") or symbol)
            fee = float(network.get("withdraw_fee_quantity") or 0.0)
            bithumb_fee_networks[net_name] = min(bithumb_fee_networks.get(net_name, float("inf")), fee)
    bithumb_withdraw_ok = False
    bithumb_status = bithumb_statuses.get("data", {}).get(symbol)
    if bithumb_status:
        bithumb_withdraw_ok = str(bithumb_status.get("withdrawal_status", "")).lower() in {"1", "true", "working", "withdraw_only"}

    upbit_wallet_status = http_json(
        "https://api.upbit.com/v1/status/wallet",
        {"Authorization": "Bearer " + upbit_jwt(), "accept": "application/json"},
    )
    upbit_fee_networks = {}
    for row in upbit_wallet_status:
        if row.get("currency") != symbol:
            continue
        if row.get("wallet_state") not in ("working", "withdraw_only", ""):
            continue
        net_type = row.get("net_type", "")
        query = urllib.parse.urlencode({"currency": symbol, "net_type": net_type}) if net_type else urllib.parse.urlencode({"currency": symbol})
        chance = http_json(
            "https://api.upbit.com/v1/withdraws/chance?" + query,
            {"Authorization": "Bearer " + upbit_jwt(query), "accept": "application/json"},
        )
        upbit_fee_networks[normalize_chain(net_type or symbol)] = float(chance.get("currency", {}).get("withdraw_fee") or 0.0)

    bybit_coin_info = http_json("https://api.bybit.com/v5/asset/coin/query-info", bybit_headers(""))
    bybit_networks = set()
    for row in bybit_coin_info["result"]["rows"]:
        if row.get("coin") != symbol:
            continue
        for chain in row.get("chains", []):
            if str(chain.get("chainDeposit", "")).lower() in {"1", "true"}:
                bybit_networks.add(normalize_chain(chain.get("chain") or chain.get("chainType") or symbol))

    okx_currencies, okx_currency_error = safe_http_json(
        "https://www.okx.com/api/v5/asset/currencies",
        okx_headers("GET", "/api/v5/asset/currencies"),
    )
    okx_networks = set()
    if okx_currencies:
        for row in okx_currencies["data"]:
            if row.get("ccy") != symbol:
                continue
            if not (row.get("canDep") is True or str(row.get("canDep")).lower() in {"true", "1"}):
                continue
            chain = row.get("chain", symbol)
            okx_networks.add(normalize_chain(chain.split("-", 1)[1] if "-" in chain else chain))

    bi_quote = get_bithumb_quote(symbol)
    up_quote = get_upbit_quote(symbol)
    by_quote = get_bybit_quote(symbol)
    ok_quote = get_okx_quote(symbol)
    bi_usdt = get_bithumb_quote("USDT")
    up_usdt = get_upbit_quote("USDT")

    pairs = []
    pair_inputs = [
        ("Bi-By", "Bithumb", "Bybit", bi_quote, by_quote, bi_usdt["ask"], bithumb_fee_networks, bybit_networks, bithumb_withdraw_ok),
        ("Bi-Ok", "Bithumb", "OKX", bi_quote, ok_quote, bi_usdt["ask"], bithumb_fee_networks, okx_networks, bithumb_withdraw_ok),
        ("Up-By", "Upbit", "Bybit", up_quote, by_quote, up_usdt["ask"], upbit_fee_networks, bybit_networks, True),
        ("Up-Ok", "Upbit", "OKX", up_quote, ok_quote, up_usdt["ask"], upbit_fee_networks, okx_networks, True),
    ]
    for label, korean_name, foreign_name, korean_quote, foreign_quote, rate, korean_fees, foreign_networks, withdraw_ok in pair_inputs:
        shared = sorted(set(korean_fees.keys()) & set(foreign_networks))
        route_ok = withdraw_ok and bool(shared)
        withdraw_fee = min(korean_fees[net] for net in shared) if shared else 0.0
        metrics = relay_metrics(
            korean_quote["ask"], korean_quote["ask_qty"],
            foreign_quote["bid"], foreign_quote["bid_qty"],
            rate, FEE_RATES[korean_name], FEE_RATES[foreign_name], withdraw_fee,
        )
        metrics.update({
            "label": label,
            "route_ok": route_ok,
            "shared": shared,
            "withdraw_fee": withdraw_fee,
            "entryable": route_ok and metrics["both_can_fill_target"] and metrics["match_qty"] > 0.0 and metrics["net_edge_pct"] > 0.0 and metrics["net_profit_krw"] >= MIN_NET_KRW,
            "korean_ask": korean_quote["ask"],
            "foreign_bid": foreign_quote["bid"],
        })
        pairs.append(metrics)

    best = max(pairs, key=lambda item: (item["net_profit_krw"], item["net_edge_pct"]))

    print("=== Live Step-by-Step Example (Docker dedicated IP) ===")
    print(f"Step 1. VPN: {ip_info['ip']} ({ip_info['city']}, {ip_info['country']})")
    print("Step 2. Private auth balances:")
    print(f"  Bithumb assets={sum(1 for row in bithumb_balances if float(row.get('balance', 0)) > 0)} | Upbit assets={sum(1 for row in upbit_balances if float(row.get('balance', 0)) > 0)}")
    okx_balance_status = okx_balance.get("code") if okx_balance else f"FAIL ({okx_balance_error})"
    print(f"  Bybit retCode={bybit_balance.get('retCode')} | OKX code={okx_balance_status}")
    print("Step 3. Universe counts:")
    print(f"  Bi={len(bithumb_symbols)} Up={len(upbit_symbols)} By={len(bybit_symbols)} Ok={len(okx_symbols)} | union-common={len(common_union)} | 4way-common={len(four_way_common)} | sample={symbol}")
    print("Step 4. Transfer routes + top-book metrics:")
    if okx_currency_error:
        print(f"  NOTE: OKX deposit-network lookup failed in this script: {okx_currency_error}")
    for pair in pairs:
        shared = ",".join(pair["shared"]) if pair["shared"] else "-"
        print(
            f"  {pair['label']}: route={'PASS' if pair['route_ok'] else 'BLOCK'} "
            f"shared={shared} wdFee={pair['withdraw_fee']} "
            f"Kask={pair['korean_ask']} Fbid={pair['foreign_bid']} "
            f"bothFill={'YES' if pair['both_can_fill_target'] else 'NO'} "
            f"matchQty={pair['match_qty']:.8f} netEdge={pair['net_edge_pct']:.6f}% "
            f"netKRW={pair['net_profit_krw']:.2f} entryable={'YES' if pair['entryable'] else 'NO'}"
        )
    print("Step 5. Engine-equivalent best pair:")
    print(f"  best={best['label']} | entryable={'YES' if best['entryable'] else 'NO'} | netKRW={best['net_profit_krw']:.2f} | netEdge={best['net_edge_pct']:.6f}%")


if __name__ == "__main__":
    main()
