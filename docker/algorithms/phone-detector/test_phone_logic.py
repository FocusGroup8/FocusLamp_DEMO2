import copy
import math
import unittest

from phone_logic import build_result, normalize_detections, same_frame_hand


def payload(seq=42, hand=True, x=.5, y=.5):
    return {'frame_seq': seq, 'hand_frame_seq': seq, 'hand_detected': hand,
            'hand_landmarks': [{'x': x, 'y': y, 'z': 0} for _ in range(21)] if hand else []}


class PhoneLogicTests(unittest.TestCase):
    def setUp(self):
        self.boxes = normalize_detections([[200, 150, 400, 350, .8, 67]], 640, 480, .35)

    def test_normalized_box_uses_unmirrored_image(self):
        self.assertEqual(self.boxes[0]['bbox']['x'], 200 / 640)
        self.assertEqual(self.boxes[0]['bbox']['height'], 200 / 480)

    def test_filters_non_phone_and_low_confidence(self):
        self.assertEqual(normalize_detections([[1, 2, 3, 4, .9, 64], [1, 2, 3, 4, .34, 67]],
                                              640, 480, .35), [])

    def test_boxes_clip_at_image_boundaries(self):
        result = normalize_detections([[-10, -20, 700, 500, .8, 67]], 640, 480, .35)
        self.assertEqual(result[0]['bbox'], {'x': 0, 'y': 0, 'width': 1, 'height': 1})

    def test_degenerate_box_is_ignored(self):
        self.assertEqual(normalize_detections([[5, 5, 4, 8, .8, 67]], 640, 480, .35), [])

    def test_invalid_model_output_is_error_not_no_phone(self):
        with self.assertRaises(ValueError):
            normalize_detections([[1, 2, 3, 4, math.nan, 67]], 640, 480, .35)

    def test_known_same_frame_no_hand_is_false(self):
        result = build_result(self.boxes, payload(hand=False), 640, 480)
        self.assertTrue(result['phone_detected'])
        self.assertIs(result['hand_overlap'], False)

    def test_same_frame_palm_overlap(self):
        result = build_result(self.boxes, payload(), 640, 480)
        self.assertIs(result['hand_overlap'], True)

    def test_hand_away_is_not_overlap(self):
        self.assertIs(build_result(self.boxes, payload(x=.01, y=.01), 640, 480)['hand_overlap'], False)

    def test_wrist_margin_matches_25_input_pixels(self):
        near = payload(x=(200 - 24) / 640, y=200 / 480)
        far = payload(x=(200 - 26) / 640, y=200 / 480)
        self.assertIs(build_result(self.boxes, near, 640, 480)['hand_overlap'], True)
        self.assertIs(build_result(self.boxes, far, 640, 480)['hand_overlap'], False)

    def test_fingertip_alone_is_not_palm_overlap(self):
        value = payload(x=.01, y=.01)
        value['hand_landmarks'][8] = {'x': .5, 'y': .5}
        self.assertIs(build_result(self.boxes, value, 640, 480)['hand_overlap'], False)

    def test_wrong_frame_is_unknown_not_false(self):
        value = payload()
        value['hand_frame_seq'] -= 1
        self.assertIsNone(build_result(self.boxes, value, 640, 480)['hand_overlap'])

    def test_missing_frame_is_unknown(self):
        value = payload()
        value.pop('frame_seq')
        self.assertIsNone(same_frame_hand(value))

    def test_boolean_is_not_a_frame_number(self):
        value = payload(seq=True)
        self.assertIsNone(same_frame_hand(value))

    def test_bad_points_are_unknown_even_when_phone_present(self):
        for replacement in ({'x': math.nan, 'y': .5}, {'x': True, 'y': .5},
                            {'x': -1, 'y': .5}, {'x': .5}, None):
            with self.subTest(replacement=replacement):
                value = payload()
                value['hand_landmarks'][3] = replacement
                self.assertIsNone(build_result(self.boxes, value, 640, 480)['hand_overlap'])

    def test_empty_points_for_detected_hand_are_unknown(self):
        value = payload()
        value['hand_landmarks'] = []
        self.assertIsNone(same_frame_hand(value))

    def test_contradictory_no_hand_points_are_unknown(self):
        value = payload()
        value['hand_detected'] = False
        self.assertIsNone(same_frame_hand(value))

    def test_no_phone_does_not_fabricate_confidence(self):
        result = build_result([], payload(), 640, 480)
        self.assertFalse(result['phone_detected'])
        self.assertIsNone(result['confidence'])
        self.assertIs(result['hand_overlap'], False)

    def test_inputs_are_not_modified(self):
        value = payload()
        original = copy.deepcopy(value)
        build_result(self.boxes, value, 640, 480)
        self.assertEqual(value, original)


if __name__ == '__main__':
    unittest.main()
