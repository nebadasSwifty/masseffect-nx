#!/usr/bin/env python3

import tempfile
import unittest
from pathlib import Path

import runtime_gap_resolver as resolver


class RuntimeGapResolverTests(unittest.TestCase):
    def test_observed_targets_deduplicates_in_first_seen_order(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            first = Path(directory) / "first.log"
            second = Path(directory) / "second.log"
            first.write_text(
                "Call to invalid or unregistered function at guest address 0x82aa0010\n"
                "Call to invalid or unregistered function at guest address 0x82BB0020\n"
            )
            second.write_text(
                "Call to invalid or unregistered function at guest address 0x82AA0010\n"
            )

            targets, occurrences = resolver.observed_targets([first, second])

        self.assertEqual(targets, ["0x82AA0010", "0x82BB0020"])
        self.assertEqual(occurrences, 3)

    def test_classify_report_extracts_gap_evidence(self) -> None:
        report = """
Addresses queried:
  0x82AA0018  function start: NO
      falls in a gap of 32 bytes: 0x82AA0010 - 0x82AA0030
      (after function 0x82A9FFF0, before 0x82AA0030)

------------------------------------------------------------
"""
        evidence, rejection = resolver.classify_report(report, "0x82AA0018")

        self.assertEqual(rejection, "")
        self.assertIsNotNone(evidence)
        assert evidence is not None
        self.assertEqual(evidence.offset, 8)
        self.assertEqual(evidence.gap_size, 32)

    def test_classify_report_rejects_known_function(self) -> None:
        report = "  0x82AA0018  function start: YES\n"

        evidence, rejection = resolver.classify_report(report, "0x82AA0018")

        self.assertIsNone(evidence)
        self.assertEqual(rejection, "already a discovered function start")

    def test_override_addresses_are_case_insensitive(self) -> None:
        addresses = resolver.override_addresses(
            '[functions]\n"0x82aa0010" = { }\n"0x82BB0020"={ entry = true }\n'
        )
        self.assertEqual(addresses, {"0x82AA0010", "0x82BB0020"})

    def test_apply_requires_selection_when_batch_has_multiple_candidates(self) -> None:
        candidates = [
            resolver.GapEvidence(
                "0x82AA0010", 16, "0x82AA0010", "0x82AA0020", "0x82A9FFF0", "0x82AA0020"
            ),
            resolver.GapEvidence(
                "0x82BB0010", 16, "0x82BB0010", "0x82BB0020", "0x82BAFFF0", "0x82BB0020"
            ),
        ]

        with self.assertRaises(SystemExit) as context:
            resolver.select_apply_candidate(candidates, None)

        self.assertEqual(context.exception.code, 2)
        self.assertEqual(
            resolver.select_apply_candidate(candidates, "0x82bb0010").address,
            "0x82BB0010",
        )


if __name__ == "__main__":
    unittest.main()
