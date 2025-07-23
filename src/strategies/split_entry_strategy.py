"""
Split Entry Strategy - 분할 진입/청산 전략
설정된 금액씩 2분 간격으로 진입/청산
"""
import asyncio
from typing import Dict, List, Optional
from decimal import Decimal
from datetime import datetime, timezone
from collections import defaultdict

import logging
from ..exchanges.connector import ExchangeConnector
from ..utils.premium_calculator import PremiumCalculator
from ..models import OrderRequest, OrderSide, OrderType
logger = logging.getLogger(__name__)


class SplitEntryStrategy:
    """분할 진입/청산 전략 관리
    
    만원씩 2분 간격으로 진입/청산하는 전략
    """
    
    # 거래소별 수수료 (Taker 기준)
    EXCHANGE_FEES = {
        'upbit': Decimal('0.0005'),     # 0.05%
        'bithumb': Decimal('0.0004'),   # 0.04%
        'okx': Decimal('0.0005'),        # 0.05% (VIP0 taker)
        'gate': Decimal('0.0005'),       # 0.05% (일반 taker)
    }
    
    def __init__(self, connector: ExchangeConnector, config: Dict):
        self.connector = connector
        self.calculator = PremiumCalculator()
        
        # 전략 설정 - config에서 가져오거나 기본값 사용
        strategy_config = config.get('strategy', {})
        self.entry_amount_krw = strategy_config.get('entry_amount', 10000)  # 회당 진입 금액
        self.max_amount_per_coin = self.entry_amount_krw * 10  # 코인당 최대 금액 (진입금액의 10배)
        self.max_coins = 5  # 최대 보유 가능한 코인 개수
        self.entry_interval = 120  # 진입 간격 (2분)
        self.entry_threshold = Decimal(str(strategy_config.get('entry_threshold', -0.5)))  # 진입 프리미엄
        self.exit_threshold = Decimal(str(strategy_config.get('exit_threshold', 0.5)))  # 청산 프리미엄
        
        # 포지션 추적
        self.positions = defaultdict(lambda: {
            'count': 0,
            'total_krw': 0,
            'global_exchange': None,
            'entries': [],
            'avg_entry_premium': Decimal('0'),
            'status': 'idle'  # idle, entering, holding, exiting
        })
        
        self.running = False
        self._position_lock = asyncio.Lock()  # 동시성 제어를 위한 Lock
        
    async def start(self):
        """전략 시작"""
        logger.info("Starting Split Entry Strategy...")
        self.running = True
        
        # 모니터링 시작
        asyncio.create_task(self._monitor_loop())
        
        logger.info("Split Entry Strategy started")
        
    async def stop(self):
        """전략 중지"""
        logger.info("Stopping Split Entry Strategy...")
        self.running = False
        logger.info("Split Entry Strategy stopped")
        
    async def _monitor_loop(self):
        """메인 모니터링 루프 - 실시간 역프 감지"""
        while self.running:
            try:
                # 모든 심볼 확인
                symbols = await self._get_common_symbols()
                
                # 심볼 하나씩 순차적으로 체크
                for symbol in symbols:
                    # 프리미엄 확인
                    premium_data = await self._get_best_premium(symbol)
                    if not premium_data:
                        continue
                    
                    position = self.positions[symbol]
                    
                    # 진입 조건 확인 - 역프 터지면 즉시 진입
                    if (premium_data['premium'] <= self.entry_threshold and 
                        premium_data['premium'] >= Decimal('-10.0') and  # -10% 이상만 (너무 큰 차이는 다른 코인)
                        position['total_krw'] < self.max_amount_per_coin and
                        position['status'] == 'idle' and
                        premium_data['funding_rate'] >= 0):  # 펀딩비 양수
                        
                        # Lock을 사용하여 동시성 제어
                        async with self._position_lock:
                            # 현재 활성 포지션 개수 체크
                            active_positions = sum(1 for pos in self.positions.values() if pos['count'] > 0)
                            if active_positions >= self.max_coins:
                                continue  # 최대 코인 제한
                            
                            # 포지션 상태를 즉시 변경하여 다른 심볼이 중복 진입하지 않도록
                            position['status'] = 'entering'
                        
                        logger.info(f"🎯 역프 감지! {symbol}: {premium_data['premium']:.2f}% - 즉시 진입")
                        
                        # 즉시 진입 작업 시작하고 완료 대기
                        await self._handle_entry(symbol, premium_data)
                        
                    # 청산 조건 확인
                    elif (premium_data['premium'] >= self.exit_threshold and
                          premium_data['premium'] <= Decimal('10.0') and  # +10% 이하만 (너무 큰 차이는 다른 코인)
                          position['count'] > 0 and
                          position['status'] == 'holding'):
                        
                        logger.info(f"💰 청산 시그널! {symbol}: {premium_data['premium']:.2f}% - 즉시 청산")
                        
                        # 즉시 청산 작업 시작하고 완료 대기
                        await self._handle_exit(symbol, premium_data)
                    
                    # API 제한을 위한 짧은 대기
                    await asyncio.sleep(0.2)
                
                # 전체 사이클 완료 후 대기
                await asyncio.sleep(5.0)
                
            except Exception as e:
                logger.error(f"Monitor loop error: {e}")
                await asyncio.sleep(1)
    
    async def _get_common_symbols(self) -> List[str]:
        """거래 가능한 공통 심볼 조회"""
        try:
            # 한국 거래소 심볼
            korean_symbols = set()
            for korean_exchange in ['upbit', 'bithumb']:
                if korean_exchange in self.connector.exchanges:
                    symbols = await self.connector.exchanges[korean_exchange].get_symbols()
                    if symbols:  # 심볼이 반환되었는지 확인
                        korean_symbols.update(s['symbol'] for s in symbols)
            
            # OKX, Gate 심볼
            global_symbols = set()
            for exchange in ['okx', 'gate']:
                if exchange in self.connector.exchanges:
                    symbols = await self.connector.exchanges[exchange].get_symbols()
                    if symbols:  # 심볼이 반환되었는지 확인
                        global_symbols.update(s['symbol'] for s in symbols)
            
            # 양쪽 모두 심볼이 있는지 확인
            if not korean_symbols:
                logger.warning("No symbols found from Korean exchanges")
                return []
            if not global_symbols:
                logger.warning("No symbols found from Global exchanges")
                return []
            
            # 공통 심볼
            common = list(korean_symbols & global_symbols)
            logger.info(f"Found {len(common)} common symbols between Korean and Global exchanges")
            return common
            
        except Exception as e:
            logger.error(f"Failed to get common symbols: {e}")
            return []
            
    async def _get_best_premium(self, symbol: str) -> Optional[Dict]:
        """최적의 프리미엄 데이터 조회 (호가창 기준)"""
        try:
            # 한국 거래소 중 최적 선택
            best_korean = None
            best_korean_data = None
            
            for korean_exchange in ['upbit', 'bithumb']:
                if korean_exchange not in self.connector.exchanges:
                    continue
                    
                orderbook = await self.connector.get_orderbook(symbol, korean_exchange)
                if not orderbook:
                    continue
                    
                ask = Decimal(str(orderbook['asks'][0][0]))
                bid = Decimal(str(orderbook['bids'][0][0]))
                
                if best_korean is None or ask < best_korean_data['ask']:
                    best_korean = korean_exchange
                    best_korean_data = {'ask': ask, 'bid': bid}
            
            if not best_korean:
                return None
                
            # USDT 환율 업데이트 후 가져오기
            await self.calculator.update_usdt_rate(best_korean, self.connector)
            usdt_rate = self.calculator.get_usdt_rate(best_korean)
            if not usdt_rate:
                logger.error(f"Failed to get USDT rate for {best_korean}")
                return None
                
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
                        continue  # 펀딩비 정보가 없으면 스킵
                    
                    funding = Decimal(str(funding))
                    if funding < 0:
                        continue  # 음수 펀딩비는 숏이 지불하므로 스킵
                    
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
            korean_ask_usd = best_korean_data['ask'] / usdt_rate
            premium = ((korean_ask_usd - best_price) / best_price) * 100
            
            return {
                'symbol': symbol,
                'premium': premium,
                'korean_exchange': best_korean,
                'korean_ask': best_korean_data['ask'],  # 매수가
                'korean_bid': best_korean_data['bid'],  # 매도가
                'korean_ask_usd': korean_ask_usd,
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
            # status는 이미 lock 안에서 설정됨
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
                    
                # 마지막 진입이 아니면 2분 대기
                if position['total_krw'] < self.max_amount_per_coin:
                    await asyncio.sleep(self.entry_interval)
                    
            position['status'] = 'holding'
            
        except Exception as e:
            logger.error(f"Entry handling error for {symbol}: {e}")
            position['status'] = 'idle'
                
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
                    
                # 마지막 청산이 아니면 2분 대기
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
                
    async def _execute_entry(self, symbol: str, data: Dict) -> bool:
        """실제 진입 주문 실행 - 정확한 헤지 비율 보장"""
        try:
            # 최신 USDT 환율 조회
            korean_exchange = data['korean_exchange']
            await self.calculator.update_all_usdt_rates(self.connector, [korean_exchange])
            current_usdt_rate = self.calculator.get_usdt_rate(korean_exchange)
            if not current_usdt_rate:
                logger.error("Failed to get USDT rate")
                return False
            
            # 최신 호가창 다시 가져오기 (체결 확실성)
            korean_ob = await self.connector.get_orderbook(symbol, korean_exchange)
            global_ob = await self.connector.get_orderbook(symbol, data['global_exchange'])
            
            if not korean_ob or not global_ob:
                logger.error(f"Failed to get orderbook for {symbol}")
                return False
                
            # 한국 거래소: 매도호가(ask)로 시장가 매수 - 즉시 체결
            korean_ask = Decimal(str(korean_ob['asks'][0][0]))
            
            # 해외: 매수호가(bid)로 시장가 숏 - 즉시 체결  
            global_bid = Decimal(str(global_ob['bids'][0][0]))
            
            # 단순하게: 만원씩 양쪽에서 시장가 주문
            korean_market_buy_krw = Decimal(str(self.entry_amount_krw))
            
            # 해외 거래소에서도 동일한 금액만큼 숏 (USDT 환산)
            futures_value_usd = korean_market_buy_krw / current_usdt_rate
            futures_size = futures_value_usd / global_bid
            futures_size = self._round_size(futures_size, data['global_exchange'])
            
            # Bithumb은 코인 수량으로 주문해야 하므로 계산
            korean_size = korean_market_buy_krw / korean_ask  # 추정치 (로깅/기록용)
            if korean_exchange == 'bithumb':
                korean_size = self._round_size(korean_size, korean_exchange)
            
            # 시장가 주문은 여러 호가를 먹으며 체결되므로 물량 체크 불필요
            futures_value_usd = futures_size * global_bid
            
            logger.info(f"Executing entry for {symbol}:")
            logger.info(f"  Premium: {data['premium']:.2f}%")
            logger.info(f"  USDT Rate: ₩{current_usdt_rate:,.2f}")
            logger.info(f"  {korean_exchange.upper()}: ₩{korean_market_buy_krw:,.0f} market buy @ ₩{korean_ask:,.0f}")
            logger.info(f"  {data['global_exchange'].upper()}: ${futures_value_usd:.2f} short ({futures_size:.8f} {symbol} @ ${global_bid:.2f})")
            
            # 병렬 주문 실행 (동시 체결)
            spot_order_params = {
                'exchange': korean_exchange,
                'symbol': symbol,
                'side': OrderSide.BUY,
                'order_type': OrderType.MARKET
            }
            
            if korean_exchange == 'bithumb':
                spot_order_params['size'] = korean_size
            else:  # upbit
                spot_order_params['total_krw'] = korean_market_buy_krw
            
            spot_task = asyncio.create_task(
                self.connector.place_order(OrderRequest(**spot_order_params))
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
                    # 실제 체결된 수량으로 롤백
                    rollback_params = {
                        'exchange': korean_exchange,
                        'symbol': symbol,
                        'side': OrderSide.SELL,
                        'order_type': OrderType.MARKET
                    }
                    if korean_exchange == 'bithumb':
                        rollback_params['size'] = korean_size
                    else:
                        rollback_params['total_krw'] = korean_market_buy_krw
                    
                    await self.connector.place_order(OrderRequest(**rollback_params))
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
                
            # 진입 기록 (실제 체결 수량은 주문 응답에서 확인 가능)
            actual_spot_size = spot_order.executed_size if spot_order.executed_size > 0 else korean_size
            actual_futures_size = futures_order.executed_size if futures_order.executed_size > 0 else futures_size
            
            self.positions[symbol]['entries'].append({
                'timestamp': datetime.now(timezone.utc),
                'amount': self.entry_amount_krw,
                'premium': data['premium'],
                'spot_price': data['korean_ask'],
                'futures_price': data['global_bid'],
                'spot_size': actual_spot_size,
                'futures_size': actual_futures_size
            })
            
            return True
            
        except Exception as e:
            logger.error(f"Execute entry error for {symbol}: {e}")
            return False
            
    async def _execute_exit(self, symbol: str, data: Dict) -> bool:
        """실제 청산 주문 실행 - 완벽한 체결 보장"""
        position = self.positions[symbol]
        if not position['entries']:
            return False
            
        # FIFO로 첫 진입 가져오기
        entry = position['entries'].pop(0)
        
        try:
            # 최신 호가창 가져오기
            korean_exchange = data['korean_exchange']
            korean_ob = await self.connector.get_orderbook(symbol, korean_exchange)
            global_ob = await self.connector.get_orderbook(symbol, position['global_exchange'])
            
            if not korean_ob or not global_ob:
                logger.error(f"Failed to get orderbook for exit {symbol}")
                position['entries'].insert(0, entry)
                return False
            
            # 한국 거래소: 매수호가(bid)로 시장가 매도 - 즉시 체결
            korean_bid = Decimal(str(korean_ob['bids'][0][0]))
            
            # 해외: 매도호가(ask)로 시장가 청산(buy) - 즉시 체결
            global_ask = Decimal(str(global_ob['asks'][0][0]))
            
            # 진입시 저장된 사이즈 사용
            spot_size = entry['spot_size']
            futures_size = entry['futures_size']
            
            # 시장가 주문은 여러 호가를 먹으며 체결되므로 물량 체크 불필요
            
            logger.info(f"Executing exit for {symbol}:")
            logger.info(f"  {korean_exchange.upper()}: {spot_size} @ {korean_bid:,.0f} KRW (bid)")
            logger.info(f"  {position['global_exchange']}: {futures_size} @ {global_ask:.6f} USDT (ask)")
            
            # 병렬 주문 실행 (동시 체결)
            spot_task = asyncio.create_task(
                self.connector.place_order(
                    OrderRequest(
                        exchange=korean_exchange,
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
                            exchange=korean_exchange,
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
            # 에러 발생시 진입 기록 복원
            position['entries'].insert(0, entry)
            return False
            
    def _round_size(self, size: Decimal, exchange: str = None) -> Decimal:
        """수량 단위 맞추기 - 거래소별 정밀도 또는 기본 8자리"""
        # 거래소별 정밀도 (공식 문서 기준)
        precision_map = {
            'upbit': 8,     # 8자리 지원
            'bithumb': 8,   # 2024년 2월부터 8자리로 변경 (자동매매는 여전히 4자리)
            'okx': 8,       # lotSz에 따라 다르지만 대부분 8자리까지 지원
            'gate': 8       # API에서 order_size_round 확인 필요, 일반적으로 8자리
        }
        
        precision = precision_map.get(exchange, 8)
        quantizer = Decimal('0.1') ** precision
        return size.quantize(quantizer, rounding='down')
            
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