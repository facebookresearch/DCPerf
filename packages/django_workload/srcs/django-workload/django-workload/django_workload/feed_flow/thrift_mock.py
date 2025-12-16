# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

"""
Mock Thrift Serialization - Simulates fbthrift overhead

This module mimics the serialization/deserialization patterns from production
IG Django, specifically the AdInsertion Thrift struct which has 80+ fields
requiring to_python_struct() conversions.

Research shows production does 800+ Python↔C++ transitions per request:
- 80+ fields per ad insertion
- 10 ads per request
- Each field: type inspection, buffer allocation, encoding
"""

import random
import struct
from io import BytesIO
from typing import Any, List


class MockThriftField:
    """
    Represents a single Thrift field with type inspection overhead.
    Simulates the to_python_struct() conversion cost.
    """

    FIELD_TYPES = {
        "i32": (1, struct.pack, "!i"),
        "i64": (2, struct.pack, "!q"),
        "string": (3, lambda fmt, val: val.encode("utf-8"), None),
        "bool": (4, struct.pack, "!?"),
        "double": (5, struct.pack, "!d"),
        "list": (6, None, None),
        "struct": (7, None, None),
    }

    def __init__(self, field_id: int, field_type: str, value: Any):
        self.field_id = field_id
        self.field_type = field_type
        self.value = value

    def serialize_to_buffer(self, buffer: BytesIO) -> None:
        """
        Simulate Thrift binary protocol serialization.
        Each field requires: type tag + field ID + value encoding.
        """
        type_id, pack_func, fmt = self.FIELD_TYPES[self.field_type]

        # Write field header (type + ID)
        buffer.write(struct.pack("!BH", type_id, self.field_id))

        # Write value (with type-specific encoding)
        if self.field_type == "i32":
            buffer.write(pack_func(fmt, self.value))
        elif self.field_type == "i64":
            buffer.write(pack_func(fmt, self.value))
        elif self.field_type == "string":
            encoded = pack_func(fmt, self.value)
            buffer.write(struct.pack("!I", len(encoded)))  # String length
            buffer.write(encoded)
        elif self.field_type == "bool":
            buffer.write(pack_func(fmt, self.value))
        elif self.field_type == "double":
            buffer.write(pack_func(fmt, self.value))
        elif self.field_type == "list":
            # Simplified list encoding
            buffer.write(struct.pack("!I", len(self.value)))
        elif self.field_type == "struct":
            # Nested struct requires recursive serialization
            if hasattr(self.value, "serialize"):
                self.value.serialize(buffer)


class AdInsertionThrift:
    """
    Mock AdInsertion Thrift struct with 80+ fields.
    Mimics instagram-server/distillery/ads/models.py:create_ad_insertion_object_py3lite()
    which has 80+ to_python_struct() calls.
    """

    def __init__(self):
        # Core ad fields (20 fields)
        self.ad_id = random.randint(1000000, 9999999)
        self.campaign_id = random.randint(100000, 999999)
        self.creative_id = random.randint(10000, 99999)
        self.advertiser_id = random.randint(1000, 9999)
        self.tracking_token = f"tk_{random.randint(1000000, 9999999)}"
        self.impression_id = f"imp_{random.randint(1000000, 9999999)}"
        self.delivery_id = random.randint(1000, 9999)
        self.insertion_id = random.randint(10000, 99999)
        self.ad_title = f"Ad Campaign {random.randint(1, 100)}"
        self.ad_subtitle = f"Subtitle {random.randint(1, 50)}"
        self.call_to_action = random.choice(["LEARN_MORE", "SHOP_NOW", "SIGN_UP"])
        self.destination_url = f"https://example.com/ad_{self.ad_id}"
        self.view_count = random.randint(0, 1000000)
        self.like_count = random.randint(0, 10000)
        self.comment_count = random.randint(0, 1000)
        self.share_count = random.randint(0, 500)
        self.is_video = random.choice([True, False])
        self.video_duration = random.randint(5, 60) if self.is_video else 0
        self.image_url = f"https://cdn.example.com/img_{self.creative_id}.jpg"
        self.media_type = random.choice(["PHOTO", "VIDEO", "CAROUSEL"])

        # Rating and trust info (15 fields)
        self.ads_iaw_rating_score = random.random()
        self.ig_ads_rating_score = random.random()
        self.trust_score = random.random()
        self.quality_score = random.random()
        self.relevance_score = random.random()
        self.engagement_score = random.random()
        self.conversion_score = random.random()
        self.brand_safety_score = random.random()
        self.content_score = random.random()
        self.creativity_score = random.random()
        self.authenticity_score = random.random()
        self.user_feedback_score = random.random()
        self.platform_score = random.random()
        self.advertiser_score = random.random()
        self.campaign_score = random.random()

        # Context and metadata (20 fields)
        self.house_ad_context = f"context_{random.randint(1, 100)}"
        self.cta_trust_info = f"trust_{random.randint(1, 50)}"
        self.text_trust_info = f"text_trust_{random.randint(1, 50)}"
        self.moment_info = f"moment_{random.randint(1, 30)}"
        self.brand_context = f"brand_{random.randint(1, 40)}"
        self.surface_type = random.choice(["FEED", "STORIES", "REELS", "EXPLORE"])
        self.placement_type = random.choice(["IN_STREAM", "BANNER", "INTERSTITIAL"])
        self.audience_segment = f"segment_{random.randint(1, 100)}"
        self.targeting_criteria = f"criteria_{random.randint(1, 50)}"
        self.budget_info = f"budget_{random.randint(1000, 100000)}"
        self.bid_amount = random.uniform(0.1, 10.0)
        self.pacing_strategy = random.choice(["STANDARD", "ACCELERATED"])
        self.optimization_goal = random.choice(["IMPRESSIONS", "CLICKS", "CONVERSIONS"])
        self.attribution_window = random.choice([1, 7, 28])
        self.frequency_cap = random.randint(1, 10)
        self.delivery_status = random.choice(["ACTIVE", "PAUSED", "COMPLETED"])
        self.performance_tier = random.choice(["PREMIUM", "STANDARD", "BUDGET"])
        self.inventory_source = random.choice(["OWNED", "PARTNER", "EXCHANGE"])
        self.creative_format = random.choice(["SINGLE_IMAGE", "CAROUSEL", "VIDEO"])
        self.landing_type = random.choice(["WEBSITE", "APP", "MESSENGER"])

        # Signals and features (25+ fields)
        self.user_engagement_signal = random.random()
        self.predicted_ctr = random.random()
        self.predicted_cvr = random.random()
        self.predicted_engagement = random.random()
        self.historical_performance = random.random()
        self.audience_match_score = random.random()
        self.context_relevance = random.random()
        self.time_relevance = random.random()
        self.location_relevance = random.random()
        self.device_targeting_score = random.random()
        self.demographic_match = random.random()
        self.interest_match = random.random()
        self.behavior_match = random.random()
        self.lookalike_score = random.random()
        self.retargeting_signal = random.random()
        self.brand_lift_estimate = random.random()
        self.incremental_reach = random.random()
        self.cross_device_signal = random.random()
        self.viewability_score = random.random()
        self.completion_rate_estimate = random.random()
        self.click_probability = random.random()
        self.conversion_probability = random.random()
        self.revenue_estimate = random.random()
        self.roi_estimate = random.random()
        self.competitor_analysis = random.random()

    def to_python_struct_fields(self) -> List[MockThriftField]:
        """
        Convert to Thrift fields requiring type inspection.
        Simulates 80+ to_python_struct() calls from production.
        """
        return [
            # Core fields
            MockThriftField(1, "i64", self.ad_id),
            MockThriftField(2, "i64", self.campaign_id),
            MockThriftField(3, "i64", self.creative_id),
            MockThriftField(4, "i64", self.advertiser_id),
            MockThriftField(5, "string", self.tracking_token),
            MockThriftField(6, "string", self.impression_id),
            MockThriftField(7, "i32", self.delivery_id),
            MockThriftField(8, "i32", self.insertion_id),
            MockThriftField(9, "string", self.ad_title),
            MockThriftField(10, "string", self.ad_subtitle),
            MockThriftField(11, "string", self.call_to_action),
            MockThriftField(12, "string", self.destination_url),
            MockThriftField(13, "i64", self.view_count),
            MockThriftField(14, "i64", self.like_count),
            MockThriftField(15, "i32", self.comment_count),
            MockThriftField(16, "i32", self.share_count),
            MockThriftField(17, "bool", self.is_video),
            MockThriftField(18, "i32", self.video_duration),
            MockThriftField(19, "string", self.image_url),
            MockThriftField(20, "string", self.media_type),
            # Rating fields
            MockThriftField(21, "double", self.ads_iaw_rating_score),
            MockThriftField(22, "double", self.ig_ads_rating_score),
            MockThriftField(23, "double", self.trust_score),
            MockThriftField(24, "double", self.quality_score),
            MockThriftField(25, "double", self.relevance_score),
            MockThriftField(26, "double", self.engagement_score),
            MockThriftField(27, "double", self.conversion_score),
            MockThriftField(28, "double", self.brand_safety_score),
            MockThriftField(29, "double", self.content_score),
            MockThriftField(30, "double", self.creativity_score),
            MockThriftField(31, "double", self.authenticity_score),
            MockThriftField(32, "double", self.user_feedback_score),
            MockThriftField(33, "double", self.platform_score),
            MockThriftField(34, "double", self.advertiser_score),
            MockThriftField(35, "double", self.campaign_score),
            # Context fields
            MockThriftField(36, "string", self.house_ad_context),
            MockThriftField(37, "string", self.cta_trust_info),
            MockThriftField(38, "string", self.text_trust_info),
            MockThriftField(39, "string", self.moment_info),
            MockThriftField(40, "string", self.brand_context),
            MockThriftField(41, "string", self.surface_type),
            MockThriftField(42, "string", self.placement_type),
            MockThriftField(43, "string", self.audience_segment),
            MockThriftField(44, "string", self.targeting_criteria),
            MockThriftField(45, "string", self.budget_info),
            MockThriftField(46, "double", self.bid_amount),
            MockThriftField(47, "string", self.pacing_strategy),
            MockThriftField(48, "string", self.optimization_goal),
            MockThriftField(49, "i32", self.attribution_window),
            MockThriftField(50, "i32", self.frequency_cap),
            MockThriftField(51, "string", self.delivery_status),
            MockThriftField(52, "string", self.performance_tier),
            MockThriftField(53, "string", self.inventory_source),
            MockThriftField(54, "string", self.creative_format),
            MockThriftField(55, "string", self.landing_type),
            # Signal fields
            MockThriftField(56, "double", self.user_engagement_signal),
            MockThriftField(57, "double", self.predicted_ctr),
            MockThriftField(58, "double", self.predicted_cvr),
            MockThriftField(59, "double", self.predicted_engagement),
            MockThriftField(60, "double", self.historical_performance),
            MockThriftField(61, "double", self.audience_match_score),
            MockThriftField(62, "double", self.context_relevance),
            MockThriftField(63, "double", self.time_relevance),
            MockThriftField(64, "double", self.location_relevance),
            MockThriftField(65, "double", self.device_targeting_score),
            MockThriftField(66, "double", self.demographic_match),
            MockThriftField(67, "double", self.interest_match),
            MockThriftField(68, "double", self.behavior_match),
            MockThriftField(69, "double", self.lookalike_score),
            MockThriftField(70, "double", self.retargeting_signal),
            MockThriftField(71, "double", self.brand_lift_estimate),
            MockThriftField(72, "double", self.incremental_reach),
            MockThriftField(73, "double", self.cross_device_signal),
            MockThriftField(74, "double", self.viewability_score),
            MockThriftField(75, "double", self.completion_rate_estimate),
            MockThriftField(76, "double", self.click_probability),
            MockThriftField(77, "double", self.conversion_probability),
            MockThriftField(78, "double", self.revenue_estimate),
            MockThriftField(79, "double", self.roi_estimate),
            MockThriftField(80, "double", self.competitor_analysis),
        ]

    def serialize(self) -> bytes:
        """
        Simulates thrift_binary_serialize_struct().
        Triggers 80+ type inspections and encoding operations.
        """
        buffer = BytesIO()

        # Write struct header
        buffer.write(struct.pack("!I", 0x80010000))  # Thrift version + type

        # Serialize each field (80+ operations)
        fields = self.to_python_struct_fields()
        for field in fields:
            # Each serialize call mimics Python↔C++ boundary crossing
            field.serialize_to_buffer(buffer)

        # Write struct end marker
        buffer.write(struct.pack("!B", 0))

        return buffer.getvalue()

    @classmethod
    def deserialize(cls, data: bytes) -> "AdInsertionThrift":
        """
        Simulates thrift_binary_deserialize_struct().
        Triggers 80+ field extractions and type conversions.
        """
        buffer = BytesIO(data)

        # Read struct header
        header = struct.unpack("!I", buffer.read(4))[0]

        # Create instance
        ad = cls()

        # Read fields (80+ operations with type inspection)
        while True:
            try:
                field_type = struct.unpack("!B", buffer.read(1))[0]
                if field_type == 0:  # End marker
                    break

                field_id = struct.unpack("!H", buffer.read(2))[0]

                # Type-dependent deserialization (branch misprediction!)
                if field_type == 1:  # i32
                    value = struct.unpack("!i", buffer.read(4))[0]
                elif field_type == 2:  # i64
                    value = struct.unpack("!q", buffer.read(8))[0]
                elif field_type == 3:  # string
                    str_len = struct.unpack("!I", buffer.read(4))[0]
                    value = buffer.read(str_len).decode("utf-8")
                elif field_type == 4:  # bool
                    value = struct.unpack("!?", buffer.read(1))[0]
                elif field_type == 5:  # double
                    value = struct.unpack("!d", buffer.read(8))[0]
                else:
                    # Skip unknown field
                    pass

            except Exception:
                break

        return ad


def thrift_serialize_ads(ads: List[AdInsertionThrift]) -> bytes:
    """
    Serialize multiple ads to binary.
    Simulates ads service response serialization.
    """
    buffer = BytesIO()

    # List header
    buffer.write(struct.pack("!I", len(ads)))

    # Serialize each ad
    for ad in ads:
        ad_bytes = ad.serialize()  # 80+ operations per ad
        buffer.write(struct.pack("!I", len(ad_bytes)))
        buffer.write(ad_bytes)

    return buffer.getvalue()


def thrift_deserialize_ads(data: bytes) -> List[AdInsertionThrift]:
    """
    Deserialize binary to ad list.
    Simulates ads service response deserialization.
    """
    buffer = BytesIO(data)

    # Read list length
    list_len = struct.unpack("!I", buffer.read(4))[0]

    ads = []
    for _ in range(list_len):
        # Read ad bytes
        ad_len = struct.unpack("!I", buffer.read(4))[0]
        ad_bytes = buffer.read(ad_len)

        # Deserialize ad (80+ operations)
        ad = AdInsertionThrift.deserialize(ad_bytes)
        ads.append(ad)

    return ads
