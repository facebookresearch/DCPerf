# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

import random
import string
import unicodedata

from datetime import datetime, timedelta
from itertools import cycle, islice

from cassandra.util import uuid_from_time
from django.core.management.base import BaseCommand
from django_cassandra_engine.management.commands import sync_cassandra

from django_workload.models import (
    BundleEntryModel,
    ClipChunkModel,
    ClipVideoModel,
    CommentedInboxEntryModel,
    FeedEntryModel,
    LikeInboxEntryModel,
    NewFollowerInboxEntryModel,
    UserModel,
)

_latin_chars = map(chr, range(256))
_latin_letters = [c for c in _latin_chars if unicodedata.category(c) == "Ll"]
# weighted random; mostly ascii with some latin
_letters_source = string.ascii_lowercase * 9 + "".join(_latin_letters)


def random_string(min_length=5, max_length=30, title=False):
    """A random string consisting of Latin letters, optionally title-cased"""
    result = "".join(
        [
            random.choice(_letters_source)
            for _ in range(random.randint(min_length, max_length))
        ]
    )
    return result if not title else result.title()


def random_datetime_generator(start=-1000, end=0):
    """Generator to produce an endless series of random datetime objects.

    *start* and *end* are relative values in number of days from today 00:00,
    and this generator produces random timestamps that fall between the two
    extremes (inclusive).
    """
    now = datetime.now()
    today = now.replace(hour=0, minute=0, second=0, microsecond=0)
    start, end = today + timedelta(days=start), today + timedelta(days=end)
    start, end = start.timestamp(), end.timestamp()

    while True:
        random_ts = random.uniform(start, end)
        yield datetime.fromtimestamp(random_ts)


class Command(BaseCommand):
    help = "Set up the django workload database"

    def handle(self, **options):
        print("Running syncdb for Cassandra")
        sync_cassandra.Command().execute(**options)

        spinner = cycle("|/-\\")

        print("Creating 1000 random users")
        users = []
        user_ids = []
        for i in range(10**3):
            print("\r{} {}".format(next(spinner), i), end="")
            user = UserModel(name=random_string(title=True))
            user.save()
            users.append(user)
            user_ids.append(user.id)
        print("\r      ", end="\r")

        print("Creating following relationships between these users")
        for i, user in enumerate(users):
            print("\r{} {}".format(next(spinner), i), end="")
            followers = random.sample(user_ids, random.randrange(50))
            user.following = [uuid for uuid in followers if user.id != uuid]
            user.save()
        print("\r      ", end="\r")

        print("Creating 100k random feed entries")
        random_dates = islice(random_datetime_generator(), 10**4)
        feedids = [uuid_from_time(t) for t in random_dates]
        for i, feedid in enumerate(feedids):
            print("\r{} {}".format(next(spinner), i), end="")
            entry = FeedEntryModel(
                userid=random.choice(user_ids),
                id=feedid,
                comment_count=random.randrange(10),
            )
            entry.save()
        print("\r       ", end="\r")

        print("Creating 5000 random inbox entries")
        types = (
            CommentedInboxEntryModel,
            LikeInboxEntryModel,
            NewFollowerInboxEntryModel,
        )
        random_dates = islice(random_datetime_generator(), 5000)
        inboxids = map(uuid_from_time, random_dates)
        for i, inboxid in enumerate(inboxids):
            print("\r{} {}".format(next(spinner), i), end="")
            inboxtype = random.choice(types)
            fields = {
                "userid": random.choice(user_ids),
                "id": inboxid,
                "feedentryid": random.choice(feedids),
                "comment_text": " ".join(
                    [random_string() for _ in range(random.randrange(3, 10))]
                ),
                "likerid": random.choice(user_ids),
                "followerid": random.choice(user_ids),
            }
            entry = inboxtype(**fields)
            entry.save()
        print("\r       ", end="\r")

        print("Creating 1000 random bundles")
        random_dates = islice(random_datetime_generator(), 1000)
        bundleids = map(uuid_from_time, random_dates)
        for i, bundleid in enumerate(bundleids):
            print("\r{} {}".format(next(spinner), i), end="")
            entrycount = random.randrange(2, 10)
            # pick entrycount unique feedids, not to be used again
            feedids, feedentries = feedids[:-entrycount], feedids[-entrycount:]
            entry = BundleEntryModel(
                userid=random.choice(user_ids),
                id=bundleid,
                comment_count=random.randrange(10),
                entry_ids=feedentries,
            )
            entry.save()
        print("\r       ", end="\r")

        # =============================================================
        # Clips/Reels data generation for clips.api.views.async_stream_clips_discover
        # =============================================================

        print("Creating 5000 random clip videos (Reels)")
        clip_video_ids = []
        hashtag_options = [
            "trending",
            "viral",
            "fyp",
            "reels",
            "explore",
            "funny",
            "dance",
            "music",
            "comedy",
            "food",
            "travel",
            "fashion",
            "tech",
            "sports",
            "fitness",
            "beauty",
            "diy",
            "pets",
            "nature",
            "art",
        ]

        for i in range(5000):
            print("\r{} {}".format(next(spinner), i), end="")

            # Random video duration between 5 and 90 seconds
            duration_ms = random.randint(5000, 90000)

            # Random hashtags (2-5 per video)
            num_hashtags = random.randint(2, 5)
            hashtags = random.sample(hashtag_options, num_hashtags)

            clip = ClipVideoModel(
                owner_id=random.choice(user_ids),
                title=random_string(min_length=10, max_length=50, title=True),
                description=" ".join(
                    [random_string() for _ in range(random.randrange(5, 20))]
                ),
                duration_ms=duration_ms,
                view_count=random.randint(100, 10000000),
                like_count=random.randint(10, 1000000),
                comment_count=random.randint(0, 50000),
                share_count=random.randint(0, 10000),
                thumbnail_url=f"https://cdn.example.com/clips/{i}/thumb.jpg",
                is_published=True,
                content_type=random.choice(["reel", "short_video", "clip"]),
                audio_track_id=random.choice(user_ids),
                hashtags=hashtags,
                quality_score=random.random(),
                engagement_score=random.random(),
            )
            clip.save()
            clip_video_ids.append(clip.id)
        print("\r       ", end="\r")

        print("Creating video chunks for each clip (5-40 chunks per video)")
        chunk_count = 0
        for video_idx, video_id in enumerate(clip_video_ids):
            print(
                "\r{} Video {}/{}".format(
                    next(spinner), video_idx, len(clip_video_ids)
                ),
                end="",
            )

            # Get video duration from the model
            video = ClipVideoModel.objects.get(id=video_id)
            video_duration_ms = video.duration_ms

            # Generate 5-40 chunks per video
            num_chunks = random.randint(5, 40)
            chunk_duration_ms = video_duration_ms // num_chunks

            for chunk_idx in range(num_chunks):
                start_time_ms = chunk_idx * chunk_duration_ms
                end_time_ms = min(start_time_ms + chunk_duration_ms, video_duration_ms)
                actual_duration_ms = end_time_ms - start_time_ms

                # Random chunk size (typically 100KB-2MB)
                chunk_size_bytes = random.randint(100000, 2000000)

                # Random resolution
                resolution = random.choice(["480p", "720p", "1080p", "4K"])

                # Bitrate based on resolution
                bitrate_map = {
                    "480p": random.randint(1000, 2000),
                    "720p": random.randint(2000, 4000),
                    "1080p": random.randint(4000, 8000),
                    "4K": random.randint(8000, 16000),
                }
                bitrate_kbps = bitrate_map[resolution]

                chunk_url = (
                    f"https://cdn.example.com/clips/{video_id}/chunk_{chunk_idx}.mp4"
                )

                chunk = ClipChunkModel(
                    video_id=video_id,
                    chunk_index=chunk_idx,
                    chunk_url=chunk_url,
                    chunk_size_bytes=chunk_size_bytes,
                    duration_ms=actual_duration_ms,
                    start_time_ms=start_time_ms,
                    end_time_ms=end_time_ms,
                    resolution=resolution,
                    bitrate_kbps=bitrate_kbps,
                    codec=random.choice(["h264", "h265", "av1"]),
                )
                chunk.save()
                chunk_count += 1

        print("\r       ", end="\r")
        print(
            f"Created {len(clip_video_ids)} clip videos with {chunk_count} total chunks"
        )
