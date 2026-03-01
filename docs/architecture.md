# Aggregation System Architecture

Pipeline:

SCAN REQUEST
    ↓
Entity Resolver
    ↓
State Resolver
    ↓
Capability Engine
    ↓
Response Builder

Principles:
- Derived states only
- Item scan redirects to box context
- Print available when pallet remains complete