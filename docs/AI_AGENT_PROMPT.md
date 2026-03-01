# AI Coding Agent Prompt

You are implementing a pallet aggregation backend system based on the provided specifications.

Goals:
- Replace hardcoded logic with rule-driven architecture.
- Implement Entity Resolver, State Resolver, Capability Engine.
- Requests and responses must follow provided JSON schemas.
- Business rules must NOT live in controllers; only in rule engine.

Architecture:
SCAN → Entity Resolver → State Resolver → Capability Engine → Response Builder

Key Principles:
1. States are derived, NOT stored.
2. Items are lookup-only entities.
3. Pallet completion is operator confirmation.
4. All actions must be capability-driven.
5. System must be deterministic and idempotent.

Implementation Tasks:
- Load YAML rules at startup.
- Resolve entity type from barcode.
- Derive states dynamically from DB facts.
- Evaluate capability rules.
- Return JSON response.

Deliverables:
- Modular services/classes
- Rule engine
- Unit-testable components