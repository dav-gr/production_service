Event Driven Extension

This directory adds an event-driven architecture layer to the pallet aggregation system.

Key Concepts:
- Commands represent user intent.
- Events represent accepted facts.
- Event Store is append-only and immutable.
- Projections build fast query tables.
- Export uses aggregation_tree_view projection.

Workflow:
SCAN -> COMMAND -> VALIDATION -> EVENT STORE -> PROJECTOR -> READ MODELS -> RESPONSE

Benefits:
- Full audit trail
- No state corruption
- Easy export of completed pallet trees
- AI-friendly deterministic logic