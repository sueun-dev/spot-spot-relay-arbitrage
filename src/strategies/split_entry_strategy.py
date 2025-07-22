"""
Split Entry Strategy - 분할 진입/청산 전략
만원씩 1분 간격으로 진입/청산
"""
import asyncio
from typing import Dict, List, Optional, Tuple
from decimal import Decimal
from datetime import datetime
from collections import defaultdict

from ..exchanges.connector import ExchangeConnector
from ..utils.logger import logger
from ..utils.premium_calculator import PremiumCalculator
from ..models import OrderRequest, OrderSide, OrderType


class SplitEntryStrategy:
    """분할 진입/청산 전략 관리"""
    
    # 거래소별 수수료 (Taker 기준)
    EXCHANGE_FEES = {
        'upbit': Decimal('0.0005'),     # 0.05%
        'bithumb': Decimal('0.0025'),   # 0.25%
        'okx': Decimal('0.0005'),        # 0.05% (VIP0 taker)
        'gate': Decimal('0.0005'),       # 0.05% (일반 taker)
    }
    
    # 거래소별 최소 주문 금액 (USD)
    MIN_ORDER_VALUE = {
        'upbit': Decimal('5000'),        # 5,000 KRW
        'bithumb': Decimal('1000'),      # 1,000 KRW
        'okx': Decimal('1'),             # 1 USD
        'gate': Decimal('1'),            # 1 USD
    }
    
    def __init__(self, connector: ExchangeConnector, config: Dict):
        self.connector = connector
        self.config = config
        self.calculator = PremiumCalculator()
        
        # 전략 설정
        self.entry_amount_krw = 10000  # 회당 진입 금액 (만원)
        self.max_amount_per_coin = 30000  # 코인당 최대 금액 (3만원)
        self.entry_interval = 60  # 진입 간격 (1분)
        self.entry_threshold = Decimal('-1.0')  # 진입 프리미엄 (-1%)
        self.exit_threshold = Decimal('0.1')  # 청산 프리미엄 (+0.1%)
        
        # 포지션 추적
        self.positions = defaultdict(lambda: {
            'count': 0,
            'total_krw': 0,
            'global_exchange': None,
            'entries': [],
            'avg_entry_premium': Decimal('0'),
            'status': 'idle'  # idle, entering, holding, exiting
        })
        
        # 실행 중인 작업
        self.active_tasks = {}
        self.running = False
        
        # 심볼별 거래 규칙 캐시
        self.symbol_info_cache = {}
        
    async def start(self):
        """전략 시작"""
        logger.info("Starting Split Entry Strategy...")
        self.running = True
        
        # USDT 환율 업데이트
        korean_exchanges = [name for name in self.connector.exchanges.keys() if name in ['upbit', 'bithumb']]
        await self.calculator.update_all_usdt_rates(
            self.connector, 
            korean_exchanges
        )
        
        # 심볼 정보 캐시 업데이트
        await self._update_symbol_info_cache()
        
        # 모니터링 시작
        monitor_task = asyncio.create_task(self._monitor_loop())
        self.active_tasks['monitor'] = monitor_task
        
        logger.info("Split Entry Strategy started")
        
    async def stop(self):
        """전략 중지"""
        logger.info("Stopping Split Entry Strategy...")
        self.running = False
        
        # 모든 작업 취소
        for task in self.active_tasks.values():
            if not task.done():
                task.cancel()
                
        logger.info("Split Entry Strategy stopped")
        
    async def _monitor_loop(self):
        """메인 모니터링 루프 - 실시간 역프 감지"""
        batch_size = 10  # Process 10 symbols at a time to avoid rate limits
        
        while self.running:
            try:
                # 모든 심볼 확인
                symbols = await self._get_common_symbols()
                
                # Process symbols in batches
                for i in range(0, len(symbols), batch_size):
                    batch = symbols[i:i + batch_size]
                    
                    # 병렬로 배치 심볼 체크
                    tasks = []
                    for symbol in batch:
                        # 이미 처리 중이면 스킵
                        if symbol in self.active_tasks:
                            continue
                        
                        task = asyncio.create_task(self._check_and_execute_symbol(symbol))
                        tasks.append(task)
                    
                    # 배치 완료 대기
                    if tasks:
                        await asyncio.gather(*tasks, return_exceptions=True)
                    
                    # Wait between batches to respect rate limits
                    await asyncio.sleep(1.0)
                
                # Wait before next full cycle
                await asyncio.sleep(5.0)
                
            except Exception as e:
                logger.error(f"Monitor loop error: {e}")
                await asyncio.sleep(1)
    
    async def _check_and_execute_symbol(self, symbol: str):
        """개별 심볼 체크 및 즉시 실행"""
        try:
            # 프리미엄 확인
            premium_data = await self._get_best_premium(symbol)
            if not premium_data:
                return
            
            position = self.positions[symbol]
            
            # 진입 조건 확인 - 역프 터지면 즉시 진입
            if (premium_data['premium'] <= self.entry_threshold and 
                position['total_krw'] < self.max_amount_per_coin and
                position['status'] == 'idle' and
                premium_data['funding_rate'] >= 0):  # 펀딩비 양수
                
                logger.info(f"🎯 역프 감지! {symbol}: {premium_data['premium']:.2f}% - 즉시 진입")
                
                # 진입 작업 시작
                task = asyncio.create_task(
                    self._handle_entry(symbol, premium_data)
                )
                self.active_tasks[symbol] = task
                
            # 청산 조건 확인
            elif (premium_data['premium'] >= self.exit_threshold and
                  position['count'] > 0 and
                  position['status'] == 'holding'):
                
                logger.info(f"💰 청산 시그널! {symbol}: {premium_data['premium']:.2f}% - 즉시 청산")
                
                # 청산 작업 시작
                task = asyncio.create_task(
                    self._handle_exit(symbol, premium_data)
                )
                self.active_tasks[symbol] = task
                
        except Exception as e:
            logger.debug(f"Error checking {symbol}: {e}")
                
    async def _get_common_symbols(self) -> List[str]:
        """거래 가능한 공통 심볼 조회"""
        try:
            # Upbit 심볼
            upbit = self.connector.exchanges.get('upbit')
            if not upbit:
                return []
                
            upbit_symbols = await upbit.get_symbols()
            upbit_set = {s['symbol'] for s in upbit_symbols}
            
            # OKX, Gate 심볼
            global_symbols = set()
            for exchange in ['okx', 'gate']:
                if exchange in self.connector.exchanges:
                    symbols = await self.connector.exchanges[exchange].get_symbols()
                    global_symbols.update(s['symbol'] for s in symbols)
            
            # 공통 심볼
            common = list(upbit_set & global_symbols)
            
            # 우선순위 심볼 먼저
            priority = ['BTC', 'ETH', 'XRP', 'SOL', 'DOGE']
            return [s for s in priority if s in common] + [s for s in common if s not in priority]
            
        except Exception as e:
            logger.error(f"Failed to get common symbols: {e}")
            return []
            
    async def _get_best_premium(self, symbol: str) -> Optional[Dict]:
        """최적의 프리미엄 데이터 조회 (호가창 기준)"""
        try:
            # Upbit 호가창
            upbit_orderbook = await self.connector.get_orderbook(symbol, 'upbit')
            if not upbit_orderbook:
                return None
                
            # 현물 매수가 = ask, 매도가 = bid
            upbit_ask = Decimal(str(upbit_orderbook['asks'][0][0]))
            upbit_bid = Decimal(str(upbit_orderbook['bids'][0][0]))
            
            # USDT 환율
            usdt_rate = self.calculator.get_usdt_rate('upbit')
            if not usdt_rate:
                usdt_rate = Decimal('1365')
                
            # 해외 거래소 비교
            best_global = None
            best_price = None
            best_funding = Decimal('0')
            global_ask = None
            
            for exchange in ['okx', 'gate']:
                if exchange not in self.connector.exchanges:
                    continue
                    
                try:
                    # 호가창
                    orderbook = await self.connector.get_orderbook(symbol, exchange)
                    if not orderbook:
                        continue
                        
                    # 선물 숏 진입가 = bid, 청산가 = ask
                    bid = Decimal(str(orderbook['bids'][0][0]))
                    ask = Decimal(str(orderbook['asks'][0][0]))
                    
                    # 펀딩비
                    funding = await self.connector.get_funding_rate(symbol, exchange)
                    if funding is None:
                        funding = Decimal('0')
                    else:
                        funding = Decimal(str(funding))
                    
                    # 더 좋은 가격인지 확인 (진입시 높은 가격이 유리)
                    if best_global is None or bid > best_price:
                        best_global = exchange
                        best_price = bid
                        best_funding = funding
                        global_ask = ask
                        
                except Exception as e:
                    logger.debug(f"Error getting {exchange} data for {symbol}: {e}")
                    
            if not best_global:
                return None
                
            # 프리미엄 계산 (진입 기준)
            upbit_ask_usd = upbit_ask / usdt_rate
            premium = ((upbit_ask_usd - best_price) / best_price) * 100
            
            return {
                'symbol': symbol,
                'premium': premium,
                'korean_exchange': 'upbit',
                'korean_ask': upbit_ask,  # 매수가
                'korean_bid': upbit_bid,  # 매도가
                'korean_ask_usd': upbit_ask_usd,
                'global_exchange': best_global,
                'global_bid': best_price,  # 숏 진입가
                'global_ask': global_ask,  # 숏 청산가
                'funding_rate': best_funding,
                'usdt_rate': usdt_rate
            }
            
        except Exception as e:
            logger.error(f"Failed to get premium for {symbol}: {e}")
            return None
            
    async def _handle_entry(self, symbol: str, premium_data: Dict):
        """분할 진입 처리"""
        try:
            position = self.positions[symbol]
            position['status'] = 'entering'
            position['global_exchange'] = premium_data['global_exchange']
            
            logger.info(f"Starting split entry for {symbol} "
                       f"(Premium: {premium_data['premium']:.2f}%, "
                       f"Exchange: {premium_data['global_exchange']})")
            
            while (position['total_krw'] < self.max_amount_per_coin and 
                   self.running):
                
                # 프리미엄 재확인
                current_data = await self._get_best_premium(symbol)
                if not current_data or current_data['premium'] > self.entry_threshold:
                    logger.info(f"Premium changed for {symbol}, stopping entry")
                    break
                    
                # 만원 진입
                success = await self._execute_entry(symbol, current_data)
                if success:
                    position['count'] += 1
                    position['total_krw'] += self.entry_amount_krw
                    
                    # 평균 프리미엄 업데이트
                    total_weight = sum(e['amount'] for e in position['entries'])
                    weighted_premium = sum(
                        e['premium'] * e['amount'] for e in position['entries']
                    )
                    position['avg_entry_premium'] = weighted_premium / total_weight
                    
                    logger.info(f"{symbol} Entry #{position['count']}: "
                               f"Total {position['total_krw']:,}원, "
                               f"Avg Premium: {position['avg_entry_premium']:.2f}%")
                else:
                    logger.error(f"Failed to enter position for {symbol}")
                    break
                    
                # 마지막 진입이 아니면 1분 대기
                if position['total_krw'] < self.max_amount_per_coin:
                    await asyncio.sleep(self.entry_interval)
                    
            position['status'] = 'holding'
            
        except Exception as e:
            logger.error(f"Entry handling error for {symbol}: {e}")
            position['status'] = 'idle'
        finally:
            # 작업 제거
            if symbol in self.active_tasks:
                del self.active_tasks[symbol]
                
    async def _handle_exit(self, symbol: str, premium_data: Dict):
        """분할 청산 처리"""
        try:
            position = self.positions[symbol]
            position['status'] = 'exiting'
            
            logger.info(f"Starting split exit for {symbol} "
                       f"(Premium: {premium_data['premium']:.2f}%)")
            
            while position['count'] > 0 and self.running:
                # 프리미엄 재확인
                current_data = await self._get_best_premium(symbol)
                if not current_data or current_data['premium'] < self.exit_threshold:
                    logger.info(f"Premium changed for {symbol}, stopping exit")
                    break
                    
                # 만원 청산
                success = await self._execute_exit(symbol, current_data)
                if success:
                    position['count'] -= 1
                    position['total_krw'] -= self.entry_amount_krw
                    
                    # 수익 계산
                    exit_premium = current_data['premium']
                    profit_pct = exit_premium - position['avg_entry_premium']
                    profit_krw = self.entry_amount_krw * profit_pct / 100
                    
                    logger.info(f"{symbol} Exit #{position['count']+1}: "
                               f"Remaining {position['total_krw']:,}원, "
                               f"Profit: {profit_krw:,.0f}원 ({profit_pct:.2f}%)")
                else:
                    logger.error(f"Failed to exit position for {symbol}")
                    break
                    
                # 마지막 청산이 아니면 1분 대기
                if position['count'] > 0:
                    await asyncio.sleep(self.entry_interval)
                    
            # 모두 청산되면 초기화
            if position['count'] == 0:
                position['status'] = 'idle'
                position['total_krw'] = 0
                position['entries'] = []
                position['global_exchange'] = None
                position['avg_entry_premium'] = Decimal('0')
                
        except Exception as e:
            logger.error(f"Exit handling error for {symbol}: {e}")
            position['status'] = 'holding'
        finally:
            # 작업 제거
            if symbol in self.active_tasks:
                del self.active_tasks[symbol]
                
    async def _execute_entry(self, symbol: str, data: Dict) -> bool:
        """실제 진입 주문 실행 - 정확한 헤지 비율 보장"""
        try:
            # 최신 호가창 다시 가져오기 (체결 확실성)
            upbit_ob = await self.connector.get_orderbook(symbol, 'upbit')
            global_ob = await self.connector.get_orderbook(symbol, data['global_exchange'])
            
            if not upbit_ob or not global_ob:
                logger.error(f"Failed to get orderbook for {symbol}")
                return False
            
            # 최신 USDT 환율 조회
            await self.calculator.update_all_usdt_rates(self.connector, ['upbit'])
            current_usdt_rate = self.calculator.get_usdt_rate('upbit')
            if not current_usdt_rate:
                logger.error("Failed to get USDT rate")
                return False
            
            # 업비트: 매도호가(ask)로 시장가 매수 - 즉시 체결
            upbit_ask = Decimal(str(upbit_ob['asks'][0][0]))
            upbit_ask_size = Decimal(str(upbit_ob['asks'][0][1]))
            
            # 해외: 매수호가(bid)로 시장가 숏 - 즉시 체결  
            global_bid = Decimal(str(global_ob['bids'][0][0]))
            global_bid_size = Decimal(str(global_ob['bids'][0][1]))
            
            # 정확한 헤지 계산
            # 1. 업비트에서 정확히 10,000원 시장가 매수
            upbit_market_buy_krw = Decimal(str(self.entry_amount_krw))
            
            # 2. 수수료 차감 후 실제 획득할 코인 수량 계산
            upbit_fee_rate = self.EXCHANGE_FEES.get('upbit', Decimal('0.0005'))
            upbit_size_after_fee = (upbit_market_buy_krw * (1 - upbit_fee_rate)) / upbit_ask
            upbit_size_after_fee = self._round_size(upbit_size_after_fee, 'upbit', symbol)
            
            # 3. 해당 코인의 정확한 USD 가치 계산
            coin_value_usd = upbit_size_after_fee * global_bid
            
            # 4. 해외 거래소 수수료 고려한 숏 포지션 크기 계산
            global_fee_rate = self.EXCHANGE_FEES.get(data['global_exchange'], Decimal('0.0005'))
            futures_size = coin_value_usd / (1 - global_fee_rate)
            futures_size = self._round_size(futures_size, data['global_exchange'], symbol)
            
            # 5. 최소 주문 금액 체크
            if upbit_market_buy_krw < self.MIN_ORDER_VALUE['upbit']:
                logger.warning(f"{symbol} Order value too small: ₩{upbit_market_buy_krw}")
                return False
                
            futures_value_usd = futures_size * global_bid
            if futures_value_usd < self.MIN_ORDER_VALUE.get(data['global_exchange'], Decimal('1')):
                logger.warning(f"{symbol} Futures value too small: ${futures_value_usd}")
                return False
            
            # 호가 물량 체크
            if futures_size > global_bid_size:
                logger.warning(f"{symbol} Global bid size insufficient: {futures_size} > {global_bid_size}")
                # 물량 부족시 비율 조정
                ratio = global_bid_size * Decimal('0.95') / futures_size
                futures_size = global_bid_size * Decimal('0.95')
                upbit_market_buy_krw = upbit_market_buy_krw * ratio
                upbit_size_after_fee = upbit_size_after_fee * ratio
            
            logger.info(f"Executing entry for {symbol}:")
            logger.info(f"  Premium: {data['premium']:.2f}%")
            logger.info(f"  USDT Rate: ₩{current_usdt_rate:,.2f}")
            logger.info(f"  Upbit: ₩{upbit_market_buy_krw:,.0f} market buy")
            logger.info(f"    → Fee: {upbit_fee_rate*100:.3f}% = ₩{upbit_market_buy_krw * upbit_fee_rate:.0f}")
            logger.info(f"    → Get: {upbit_size_after_fee:.8f} {symbol} @ ₩{upbit_ask:,.0f}")
            logger.info(f"  {data['global_exchange'].upper()}: {futures_size:.8f} {symbol} short @ ${global_bid:.6f}")
            logger.info(f"    → Fee: {global_fee_rate*100:.3f}% = ${coin_value_usd * global_fee_rate:.2f}")
            logger.info(f"    → Value: ${futures_value_usd:.2f} USD")
            logger.info(f"  Hedge Ratio: {(futures_value_usd / (coin_value_usd * (1 + global_fee_rate))):.4f}")
            
            # 병렬 주문 실행 (동시 체결)
            spot_task = asyncio.create_task(
                self.connector.place_order(
                    OrderRequest(
                        exchange='upbit',
                        symbol=symbol,
                        side=OrderSide.BUY,
                        size=upbit_size_after_fee,  # Not used for market buy
                        order_type=OrderType.MARKET,
                        total_krw=upbit_market_buy_krw  # This is what matters
                    )
                )
            )
            
            futures_task = asyncio.create_task(
                self.connector.place_order(
                    OrderRequest(
                        exchange=data['global_exchange'],
                        symbol=symbol,
                        side=OrderSide.SELL,
                        size=futures_size,
                        order_type=OrderType.MARKET
                    )
                )
            )
            
            # 동시 실행 및 결과 확인
            spot_order, futures_order = await asyncio.gather(spot_task, futures_task)
            
            if not spot_order or not futures_order:
                logger.error(f"Order execution failed - Spot: {spot_order is not None}, Futures: {futures_order is not None}")
                
                # 한쪽만 체결된 경우 반대 포지션 정리
                if spot_order and not futures_order:
                    await self.connector.place_order(
                        OrderRequest(
                            exchange='upbit',
                            symbol=symbol,
                            side=OrderSide.SELL,
                            size=upbit_size,
                            order_type=OrderType.MARKET
                        )
                    )
                elif futures_order and not spot_order:
                    await self.connector.place_order(
                        OrderRequest(
                            exchange=data['global_exchange'],
                            symbol=symbol,
                            side=OrderSide.BUY,
                            size=futures_size,
                            order_type=OrderType.MARKET
                        )
                    )
                return False
                
            # 진입 기록
            self.positions[symbol]['entries'].append({
                'timestamp': datetime.now(),
                'amount': self.entry_amount_krw,
                'premium': data['premium'],
                'spot_price': data['korean_ask'],
                'futures_price': data['global_bid'],
                'spot_size': upbit_size,
                'futures_size': futures_size
            })
            
            return True
            
        except Exception as e:
            logger.error(f"Execute entry error for {symbol}: {e}")
            return False
            
    async def _execute_exit(self, symbol: str, data: Dict) -> bool:
        """실제 청산 주문 실행 - 완벽한 체결 보장"""
        try:
            position = self.positions[symbol]
            if not position['entries']:
                return False
                
            # FIFO로 첫 진입 가져오기
            entry = position['entries'].pop(0)
            
            # 최신 호가창 가져오기
            upbit_ob = await self.connector.get_orderbook(symbol, 'upbit')
            global_ob = await self.connector.get_orderbook(symbol, position['global_exchange'])
            
            if not upbit_ob or not global_ob:
                logger.error(f"Failed to get orderbook for exit {symbol}")
                position['entries'].insert(0, entry)
                return False
            
            # 업비트: 매수호가(bid)로 시장가 매도 - 즉시 체결
            upbit_bid = Decimal(str(upbit_ob['bids'][0][0]))
            upbit_bid_size = Decimal(str(upbit_ob['bids'][0][1]))
            
            # 해외: 매도호가(ask)로 시장가 청산(buy) - 즉시 체결
            global_ask = Decimal(str(global_ob['asks'][0][0]))
            global_ask_size = Decimal(str(global_ob['asks'][0][1]))
            
            # 호가 물량 체크
            spot_size = entry['spot_size']
            futures_size = entry['futures_size']
            
            if spot_size > upbit_bid_size:
                logger.warning(f"{symbol} Upbit bid size insufficient for exit")
                spot_size = upbit_bid_size * Decimal('0.95')
                
            if futures_size > global_ask_size:
                logger.warning(f"{symbol} Global ask size insufficient for exit")
                futures_size = global_ask_size * Decimal('0.95')
            
            logger.info(f"Executing exit for {symbol}:")
            logger.info(f"  Upbit: {spot_size} @ {upbit_bid:,.0f} KRW (bid)")
            logger.info(f"  {position['global_exchange']}: {futures_size} @ {global_ask:.6f} USDT (ask)")
            
            # 병렬 주문 실행 (동시 체결)
            spot_task = asyncio.create_task(
                self.connector.place_order(
                    OrderRequest(
                        exchange='upbit',
                        symbol=symbol,
                        side=OrderSide.SELL,
                        size=spot_size,
                        order_type=OrderType.MARKET
                    )
                )
            )
            
            futures_task = asyncio.create_task(
                self.connector.place_order(
                    OrderRequest(
                        exchange=position['global_exchange'],
                        symbol=symbol,
                        side=OrderSide.BUY,
                        size=futures_size,
                        order_type=OrderType.MARKET
                    )
                )
            )
            
            # 동시 실행 및 결과 확인
            spot_order, futures_order = await asyncio.gather(spot_task, futures_task)
            
            if not spot_order or not futures_order:
                logger.error(f"Exit execution failed - Spot: {spot_order is not None}, Futures: {futures_order is not None}")
                
                # 한쪽만 체결된 경우 원상복구
                if spot_order and not futures_order:
                    await self.connector.place_order(
                        OrderRequest(
                            exchange='upbit',
                            symbol=symbol,
                            side=OrderSide.BUY,
                            size=spot_size,
                            order_type=OrderType.MARKET
                        )
                    )
                elif futures_order and not spot_order:
                    await self.connector.place_order(
                        OrderRequest(
                            exchange=position['global_exchange'],
                            symbol=symbol,
                            side=OrderSide.SELL,
                            size=futures_size,
                            order_type=OrderType.MARKET
                        )
                    )
                    
                # 진입 기록 복원
                position['entries'].insert(0, entry)
                return False
                
            return True
            
        except Exception as e:
            logger.error(f"Execute exit error for {symbol}: {e}")
            return False
            
    async def _update_symbol_info_cache(self):
        """거래소별 심볼 정보 캐시 업데이트"""
        try:
            for exchange_name, exchange in self.connector.exchanges.items():
                symbols = await exchange.get_symbols()
                for symbol_info in symbols:
                    symbol = symbol_info['symbol']
                    if symbol not in self.symbol_info_cache:
                        self.symbol_info_cache[symbol] = {}
                    
                    # 수량 단위 저장
                    self.symbol_info_cache[symbol][exchange_name] = {
                        'size_precision': symbol_info.get('size_precision', 8),
                        'price_precision': symbol_info.get('price_precision', 2),
                        'min_size': symbol_info.get('min_size', Decimal('0.0001')),
                        'min_notional': symbol_info.get('min_notional', Decimal('1'))  # 최소 거래 금액
                    }
            
            logger.info(f"Updated symbol info cache for {len(self.symbol_info_cache)} symbols")
        except Exception as e:
            logger.error(f"Failed to update symbol info cache: {e}")
    
    def _round_size(self, size: Decimal, exchange: str, symbol: str) -> Decimal:
        """거래소별 수량 단위 맞추기"""
        try:
            # 캐시에서 정보 가져오기
            if symbol in self.symbol_info_cache and exchange in self.symbol_info_cache[symbol]:
                precision = self.symbol_info_cache[symbol][exchange]['size_precision']
                min_size = self.symbol_info_cache[symbol][exchange]['min_size']
                
                # 소수점 자리수로 변환
                if precision == 0:
                    quantum = Decimal('1')
                else:
                    quantum = Decimal(f'0.{"0" * (precision - 1)}1')
                
                # 수량 단위 맞추기 (내림)
                rounded_size = size.quantize(quantum, rounding='down')
                
                # 최소 수량 체크
                if rounded_size < min_size:
                    return Decimal('0')
                
                return rounded_size
            else:
                # 캐시에 없으면 기본값 사용
                if symbol == 'BTC':
                    return size.quantize(Decimal('0.00001'), rounding='down')
                elif symbol == 'ETH':
                    return size.quantize(Decimal('0.0001'), rounding='down')
                else:
                    return size.quantize(Decimal('0.01'), rounding='down')
                
        except Exception as e:
            logger.error(f"Error rounding size for {symbol} on {exchange}: {e}")
            return size.quantize(Decimal('0.01'), rounding='down')
            
    def get_positions_summary(self) -> Dict:
        """현재 포지션 요약"""
        summary = {
            'total_positions': 0,
            'total_krw': 0,
            'positions': {}
        }
        
        for symbol, pos in self.positions.items():
            if pos['count'] > 0:
                summary['total_positions'] += pos['count']
                summary['total_krw'] += pos['total_krw']
                summary['positions'][symbol] = {
                    'count': pos['count'],
                    'total_krw': pos['total_krw'],
                    'avg_premium': float(pos['avg_entry_premium']),
                    'exchange': pos['global_exchange'],
                    'status': pos['status']
                }
                
        return summary