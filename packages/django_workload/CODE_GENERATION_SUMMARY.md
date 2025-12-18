# Code Variant Generation Summary

## ✅ Generation Complete!

Successfully generated code variants using fixed random seed for reproducibility.

## Configuration

- **Random Seed**: 42 (for reproducibility)
- **Feed Timeline Variants**: 20
- **Step Variants Per Type**: 10
- **Total Step Types**: 6 (SourceAndRankStep, FetchAdsStep, InsertAdsStep, TimelineStep, BrandSafetyStep, ViewStateStep)

## Generated Files

### 1. Feed Timeline Variants (20 files)
- `feed_timeline_v0.py` through `feed_timeline_v19.py`
- Each variant has a unique permutation of:
  - 3-6 FeedFlow steps
  - Different ordering of steps
  - Different CPU primitive selections
  - Different iteration counts

### 2. FeedFlow Step Variants (10 files)
- `feed_flow/steps_v0.py` through `feed_flow/steps_v9.py`
- Each file contains variants for all 6 step types
- Each variant pre-permutes:
  - 2-5 CPU primitives (string_manipulation, json_operations, list_operations, dict_operations, math_operations, sorting_operations, regex_operations, compression_operations)
  - Specific RPC call sequences
  - Operation sizes (100, 500, 1000)

### 3. URL Configuration
- `urls_generated.py` - Contains URL patterns for all 21 endpoints (1 original + 20 variants)

### 4. Client URLs Template
- `client/urls_template.txt` - List of all endpoints for load testing
- Total: 21 endpoints

## Key Benefits

### 1. **Larger I-Cache Footprint**
- 20 different feed_timeline implementations
- 60 different step implementations (10 variants × 6 step types)
- Pre-generated permutations avoid code path optimization

### 2. **Reduced RNG Overhead**
- CPU primitives are pre-permuted at code generation time
- No runtime random selection needed
- Fixed sequences for consistent behavior

### 3. **Balanced Workload Composition**
- Each variant mixes CPU primitives and RPC calls
- Different ratios of computation vs I/O
- Varied operation sizes and iteration counts

### 4. **Reproducibility**
- Fixed random seed (42) ensures identical generation
- Can regenerate exact same code variants
- Consistent across different environments

## Example Variant Structure

### Feed Timeline Variant 0
```python
# Steps: ViewStateStepV4, BrandSafetyStepV1, FetchAdsStepV5, InsertAdsStepV3

def feed_timeline_v0(request):
    flow = FeedFlow(context)

    flow.add_step(ViewStateStepV4(
        variant_id=4,
        primitives=['sorting_operations', 'dict_operations', 'json_operations'],
        num_iterations=2
    ))

    flow.add_step(BrandSafetyStepV1(
        variant_id=1,
        primitives=['regex_operations', 'compression_operations', 'string_manipulation', 'math_operations'],
        num_iterations=1
    ))
    # ... more steps
```

### Step Variant Example
```python
class FetchAdsStepV5(FeedFlowStep):
    def prepare(self, context):
        # Pre-permuted primitive sequence
        list_operations(size=500)
        json_operations(size=1000)
        dict_operations(size=100)
        json_operations(size=500)
        list_operations(size=1000)
        # ... more primitives

    def run(self, context):
        # Pre-permuted RPC call sequence
        ads_response = self.ads_client.fetch_ads(context.user_id, 50)
        context.ads = [ad for ad in ads_response.ads]
        return context.data
```

## Next Steps

### 1. Update Django URLs
Add to main `urls.py`:
```python
from django.urls import path, include

urlpatterns = [
    # Existing patterns...
    path('workload/', include('django_workload.urls_generated')),
]
```

### 2. Update Views
Import all generated view functions in `views.py`:
```python
from .feed_timeline_v0 import feed_timeline_v0
from .feed_timeline_v1 import feed_timeline_v1
# ... import all 20 variants
```

### 3. Update Client
Use the generated `urls_template.txt` for load testing:
```bash
siege -b -r 1000 -c 68 -f /path/to/client/urls_template.txt
```

## Performance Expectations

### I-Cache Pressure
- **Before**: Single feed_timeline implementation (~2KB code)
- **After**: 20 variants + 60 step variants (~100KB+ code)
- **Expected MPKI increase**: 2-3× due to larger code footprint

### CPU Utilization
- **Before**: 65% (RPC-dominated)
- **After**: 75-85% (balanced CPU + RPC)
- **More CPU primitives** mixed with RPC calls should increase compute intensity

### Workload Distribution
- Different variants stress different code paths
- Load balancer distributes requests across all variants
- Better representation of production microservice diversity

## Regeneration

To regenerate with different parameters:

```bash
# Change configuration in generate_code_variants.py
NUM_FEED_TIMELINE_VARIANTS = 40  # More variants
NUM_STEP_VARIANTS = 15           # More step variants
RANDOM_SEED = 123                # Different seed

# Run generation
python3 generate_code_variants.py
```

## Files Location

All generated files are in:
```
/data/users/wsu/fbsource/fbcode/cea/chips/benchpress/packages/django_workload/srcs/django-workload/django-workload/django_workload/
├── feed_timeline_v0.py ... feed_timeline_v19.py
├── urls_generated.py
└── feed_flow/
    └── steps_v0.py ... steps_v9.py
```

Client URLs template:
```
/data/users/wsu/fbsource/fbcode/cea/chips/benchpress/packages/django_workload/srcs/srcs/django-workload/client/urls_template.txt
```
