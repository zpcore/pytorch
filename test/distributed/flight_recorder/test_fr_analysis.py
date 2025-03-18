# Owner(s): ["oncall: distributed"]

import math
import pathlib
import sys


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent.parent

sys.path.insert(0, str(REPO_ROOT))
from tools.flight_recorder.components.types import COLLECTIVES, MatchInfo, MatchState
from tools.flight_recorder.components.utils import match_one_event


# Make sure to remove REPO_ROOT after import is done
sys.path.remove(str(REPO_ROOT))

from torch.testing._internal.common_utils import run_tests, TestCase


def create_one_event(
    collective_name,
    pg_info,
    input_sizes,
    output_sizes,
    state="scheduled",
    collective_seq_id=0,
    p2p_seq_id=0,
    output_dtypes="float32",
):
    return {
        "profiling_name": f"nccl:{collective_name}",
        "state": state,
        "process_group": pg_info,
        "input_sizes": input_sizes,
        "output_sizes": output_sizes,
        "input_dtypes": "float32",
        "output_dtypes": output_dtypes,
        "collective_seq_id": str(collective_seq_id),
        "p2p_seq_id": str(p2p_seq_id),
        "time_created_ns": 0,
        "frames": [],
    }


class FlightRecorderEventTest(TestCase):
    def test_match_one_event(self):
        e1 = create_one_event(
            "all_reduce", ("0", "default"), [[4, 4]], [[4, 4]], "scheduled", 1
        )
        membership = {"0": {0, 1}}
        self.assertEqual(
            match_one_event(e1, e1, membership, "0").state, MatchState.FULLY_MATCHED
        )

        e2 = create_one_event(
            "all_gather", ("0", "default"), [[4, 4]], [[4, 4]], "scheduled", 1
        )
        self.assertEqual(
            match_one_event(e1, e2, membership, "0").state,
            MatchState.COLLECTIVE_TYPE_MISMATCH,
        )

        e3 = create_one_event(
            "all_to_all", ("0", "default"), [[4, 4]], [[4, 4]], "scheduled", 1
        )
        e4 = create_one_event(
            "all_to_all", ("0", "default"), [[4, 4]], [[4, 4]], "scheduled", 1
        )
        self.assertEqual(
            match_one_event(e3, e4, membership, "0").state, MatchState.UNDECIDED
        )

        e5 = create_one_event(
            "all_reduce", ("0", "default"), [[5, 4]], [[4, 4]], "scheduled", 1, 1
        )
        self.assertEqual(
            match_one_event(e1, e5, membership, "0").state,
            MatchState.SIZE_OR_SYNTAX_MISMATCH,
        )

        e6 = create_one_event(
            "all_reduce", ("0", "default"), [[4, 4]], [[5, 4]], "scheduled", 1, 2
        )
        self.assertEqual(
            match_one_event(e1, e6, membership, "0").state,
            MatchState.SIZE_OR_SYNTAX_MISMATCH,
        )

        e7 = create_one_event(
            "all_reduce", ("0", "default"), [[4, 4]], [[5, 4]], "scheduled", 2
        )
        self.assertEqual(
            match_one_event(e7, e7, membership, "0").state,
            MatchState.SIZE_OR_SYNTAX_MISMATCH,
        )

        e9 = create_one_event(
            "all_reduce", ("0", "default"), [[4, 4]], [[4, 4]], "completed", 1
        )
        self.assertEqual(
            match_one_event(e1, e9, membership, "0").state,
            MatchState.COLLECTIVE_STATE_MISMATCH,
        )

        e10 = create_one_event(
            "all_reduce",
            ("0", "default"),
            [[4, 4]],
            [[4, 4]],
            "completed",
            1,
            output_dtypes="float16",
        )
        self.assertEqual(
            match_one_event(e10, e9, membership, "0").state,
            MatchState.COLLECTIVE_DTYPE_MISMATCH,
        )

    def test_all_events(self):
        for collective in sorted(COLLECTIVES):
            input_sizes = [[4, 4]]
            output_sizes = [[4, 4]]
            expectedState = MatchState.FULLY_MATCHED
            if collective == "_reduce_scatter_base":
                input_sizes = [[4, 4]]
                output_sizes = [[input_sizes[0][0] * 2]]
            if collective == "all_gather":
                output_sizes = [[math.prod(input_sizes[0]) * 2]]
            if collective == "all_to_all":
                expectedState = MatchState.UNDECIDED
            event = create_one_event(
                collective, ("0", "default"), input_sizes, output_sizes, "scheduled", 1
            )
            membership = {"0": {0, 1}}
            result = match_one_event(event, event, membership, "0").state
            self.assertEqual(result, expectedState)


class FlightMatchInfoTest(TestCase):
    def test_match_info(self):
        m1 = MatchInfo(MatchState.FULLY_MATCHED, "rank 0")
        m2 = MatchInfo(MatchState.FULLY_MATCHED, "rank 1")
        self.assertEqual(m1.state, MatchState.FULLY_MATCHED)
        self.assertEqual(m1.state, m2.state)
        self.assertEqual(str(m1), "Error type: FULLY_MATCHED, rank 0")
        self.assertEqual(str(m2), "Error type: FULLY_MATCHED, rank 1")


if __name__ == "__main__":
    run_tests()
