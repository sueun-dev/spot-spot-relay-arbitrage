#!/bin/bash
# Kimchi Premium Trading Bot - Direct Trading Script

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

clear

echo -e "${BLUE}================================================${NC}"
echo -e "${BLUE}   🚀 Kimchi Premium Auto Trading Bot 🚀${NC}"
echo -e "${BLUE}================================================${NC}"
echo ""
echo -e "${YELLOW}⚠️  WARNING: This bot trades with REAL MONEY!${NC}"
echo -e "${YELLOW}⚠️  진짜 돈으로 거래합니다!${NC}"
echo ""
echo -e "Trading Strategy:"
echo -e "  • Entry: ≤ -1.0% reverse premium"
echo -e "  • Exit: ≥ +0.1% premium"
echo -e "  • Amount: ₩10,000 × 3 (split entry)"
echo -e "  • Max per coin: ₩30,000"
echo ""

# Check if .env exists
if [ ! -f .env ]; then
    echo -e "${RED}❌ Error: .env file not found!${NC}"
    echo -e "Create .env file with your API keys first."
    exit 1
fi

# Ask for confirmation
echo -e "${YELLOW}Do you want to start trading? (yes/no)${NC}"
read -p "> " confirm

if [ "$confirm" != "yes" ]; then
    echo -e "${RED}Trading cancelled.${NC}"
    exit 0
fi

echo ""
echo -e "${GREEN}Starting trading bot...${NC}"
echo -e "${YELLOW}Press Ctrl+C to stop${NC}"
echo ""

# Run with connection test first
uv run python main.py --test