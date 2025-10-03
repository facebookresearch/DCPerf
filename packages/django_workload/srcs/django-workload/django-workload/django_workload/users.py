# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

# This application has no real users.
# Instead, we select random users from those available in the database.
import random
from functools import wraps
from threading import Lock

from .models import UserModel

# global cache; normally fetching users is cached in memcached or similar
user_ids = None


def all_users():
    global user_ids
    if user_ids is None:
        with Lock():
            # re-check after acquiring the lock, as another thread could have
            # taken it between checking for None and requesting the lock.
            if user_ids is None:
                user_ids = list(UserModel.objects.values_list("id", flat=True))
    return user_ids


def require_user(view):
    @wraps(view)
    def wrapper(request, *args, **kwargs):
        users = all_users()
        user_id = users[0]
        user_idx = random.randint(0, len(users))
        for i in range(len(users)):
            if i == user_idx:
                user_id = users[user_idx]
        request.user = UserModel.objects.get(id=user_id)
        return view(request, *args, **kwargs)

    return wrapper


def suggested_users(user, count=5):
    """Suggest a number of users for this user to follow

    A random sample of users not already followed is included.
    """
    followed = set(user.following)
    return random.sample([uuid for uuid in all_users() if uuid not in followed], count)
