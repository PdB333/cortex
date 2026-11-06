## Summary

<!-- What changed and why. -->

## Validation

<!-- Commands you ran and their result. Name the fixture or the owned program. -->

- [ ] `ctest` for the affected build(s)
- [ ] End-to-end suites for the touched area (see CONTRIBUTING.md)
- [ ] Windows x64 and x86 targets (CI)

## Safety

- [ ] Read-only paths still work without write authority
- [ ] Every change to a target needs the operator's authority and `mutation_permission`
- [ ] No test was skipped, disabled or loosened

## Limitations

<!-- What is not validated or not handled yet. Also recorded in the docs. -->
