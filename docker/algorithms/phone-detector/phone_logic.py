"""Phone boxes and same-frame hand overlap, without model or network imports.

Overlap is a 2-D image observation. It is not proof of physical contact, phone
use, or gaming. All coordinates here refer to the unmirrored input image.
"""
import math

PHONE_CLASS_ID = 67
HAND_KEY_LANDMARKS = (0, 5, 9, 13, 17)


def finite_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def frame_number(value):
    return isinstance(value, int) and not isinstance(value, bool) and value >= 0


def normalize_detections(rows, width, height, threshold):
    """Convert YOLO xyxy/conf/class rows to normalized phone-only boxes."""
    if not finite_number(width) or not finite_number(height) or width <= 0 or height <= 0:
        raise ValueError('Invalid image dimensions')
    detections = []
    for row in rows:
        if len(row) < 6 or not all(finite_number(value) for value in row[:6]):
            raise ValueError('Invalid model detection')
        x1, y1, x2, y2, confidence, class_id = row[:6]
        if not 0 <= confidence <= 1:
            raise ValueError('Invalid model confidence')
        if class_id != PHONE_CLASS_ID or confidence < threshold:
            continue
        x1, x2 = max(0.0, min(width, x1)), max(0.0, min(width, x2))
        y1, y2 = max(0.0, min(height, y1)), max(0.0, min(height, y2))
        if x2 <= x1 or y2 <= y1:
            continue
        detections.append({
            'bbox': {'x': x1 / width, 'y': y1 / height,
                     'width': (x2 - x1) / width, 'height': (y2 - y1) / height},
            'confidence': float(confidence),
        })
    return detections


def same_frame_hand(payload):
    """Return [] for observed no-hand, 21 points for a hand, None if unknown."""
    seq, hand_seq = payload.get('frame_seq'), payload.get('hand_frame_seq')
    if not frame_number(seq) or not frame_number(hand_seq) or seq != hand_seq:
        return None
    detected, points = payload.get('hand_detected'), payload.get('hand_landmarks')
    if detected is False:
        return [] if points == [] else None
    if detected is not True or not isinstance(points, list) or len(points) != 21:
        return None
    for point in points:
        if not isinstance(point, dict):
            return None
        if any(not finite_number(point.get(key)) or not 0 <= point[key] <= 1 for key in ('x', 'y')):
            return None
    return points


def build_result(detections, payload, width, height, margin_px=25.0):
    """Use the old palm/wrist rule with a 25-pixel margin on the input image."""
    points = same_frame_hand(payload)
    overlap = None if points is None else False
    if points:
        mx, my = margin_px / width, margin_px / height
        for detection in detections:
            box = detection['bbox']
            if any(box['x'] - mx <= points[index]['x'] <= box['x'] + box['width'] + mx
                   and box['y'] - my <= points[index]['y'] <= box['y'] + box['height'] + my
                   for index in HAND_KEY_LANDMARKS):
                overlap = True
                break
    return {'phone_detected': bool(detections), 'hand_overlap': overlap,
            'confidence': max((item['confidence'] for item in detections), default=None),
            'detections': detections}
