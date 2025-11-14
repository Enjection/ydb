# Manual Parameterized Test Demo

This is a demonstration test suite for testing parameter reading functionality with `ya make`.

## Purpose

This test suite demonstrates:
1. Reading string and integer parameters from `--test-param`
2. Implementing different sampling strategies for exhaustive permutation testing
3. Testing actual SchemeShard operations with configurable order

## Running the Tests

### 1. Simple Parameter Reading

Test basic parameter reading:

```bash
cd ydb/core/tx/schemeshard/ut_param_test

# With default parameters
ya make -ttt --test-tag=ya:manual

# With custom parameters
ya make -ttt --test-tag=ya:manual \
    --test-param my_string=hello \
    --test-param my_number=42
```

### 2. Permutation Sampling Demo

Test different sampling strategies:

```bash
# Test all permutations (default)
ya make -ttt --test-tag=ya:manual

# Random sampling - 10 random permutations
ya make -ttt --test-tag=ya:manual \
    --test-param max_permutations=10 \
    --test-param sampling_strategy=random

# Distributed sampling - evenly spaced through permutation space
ya make -ttt --test-tag=ya:manual \
    --test-param max_permutations=10 \
    --test-param sampling_strategy=distributed

# First N permutations
ya make -ttt --test-tag=ya:manual \
    --test-param max_permutations=10 \
    --test-param sampling_strategy=first
```

### 3. Actual SchemeShard Test with Custom Order

Test creating sequences in a specific order:

```bash
# Default order (0, 1, 2, 3)
ya make -ttt --test-tag=ya:manual

# Custom order
ya make -ttt --test-tag=ya:manual \
    --test-param operation_order=3,1,0,2

# Another custom order
ya make -ttt --test-tag=ya:manual \
    --test-param operation_order=2,3,0,1
```

### Run Without Rebuilding

If already built:

```bash
ya make -r -ttt --test-tag=ya:manual \
    --test-param max_permutations=10 \
    --test-param sampling_strategy=random
```

## Test Descriptions

### SimpleParameterReading

Demonstrates basic parameter reading:
- Reads `my_string` parameter (default: "default_value")
- Reads `my_number` parameter (default: 123)
- Prints the values to stderr

### PermutationSamplingDemo

Demonstrates sampling strategies for exhaustive testing:
- Shows how different sampling strategies work
- Simulates testing permutations of 4 operations (24 total)
- Prints which permutations would be tested

### ActualSchemeShardTest

Real SchemeShard test that creates sequences:
- Creates 4 sequences in configurable order
- Verifies all sequences are created successfully
- Useful for testing operation order dependencies

## Expected Output

When running with parameters, you should see output like:

```
========================================
PARAMETER TEST RESULTS:
  my_string = hello
  my_number = 42
========================================
```

## Notes

- All tests are marked with `ya:manual` tag and won't run in regular CI
- Tests use environment variables prefixed with `TEST_PARAM_` **in UPPERCASE**
  - `--test-param max_permutations=100` becomes `TEST_PARAM_MAX_PERMUTATIONS`
  - The helper functions handle this conversion automatically
- The sampling demo shows what an exhaustive test framework would do
- The actual SchemeShard test demonstrates real-world usage

## Next Steps

This test demonstrates the concepts that will be used in the full implementation:
1. Parameter reading from test invocation
2. Configurable sampling strategies
3. Real SchemeShard operation testing with variable orders
