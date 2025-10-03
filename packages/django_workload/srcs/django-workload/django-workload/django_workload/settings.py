# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

import os

# Build paths inside the project like this: os.path.join(BASE_DIR, ...)
BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# Quick-start development settings - unsuitable for production
# See https://docs.djangoproject.com/en/1.11/howto/deployment/checklist/

# SECURITY WARNING: keep the secret key used in production secret!
SECRET_KEY = "()2uyyko+p=dv*nmu$b5my9px!e0=6r5unm19or$02$-c62%gb"

# SECURITY WARNING: don't run with debug turned on in production!
DEBUG = True

ALLOWED_HOSTS = []

# Sample rate for profiling
SAMPLE_RATE = 1000

# Enable/Disable profiling
PROFILING = True

# Application definition

INSTALLED_APPS = [
    "django.contrib.admin",
    "django.contrib.auth",
    "django.contrib.contenttypes",
    "django.contrib.sessions",
    "django.contrib.messages",
    "django.contrib.staticfiles",
    "django_cassandra_engine",
    "django_workload",
]

MIDDLEWARE = [
    #    'django.middleware.security.SecurityMiddleware',
    "django.contrib.sessions.middleware.SessionMiddleware",
    "django.middleware.common.CommonMiddleware",
    #    'django.middleware.csrf.CsrfViewMiddleware',
    "django.contrib.auth.middleware.AuthenticationMiddleware",
    "django.contrib.messages.middleware.MessageMiddleware",
    #    'django.middleware.clickjacking.XFrameOptionsMiddleware',
    "django_workload.middleware.global_request_middleware",
    "django_workload.middleware.memory_cpu_stats_middleware",
    "django_workload.middleware.GraphiteRequestTimingMiddleware",
    "django_workload.middleware.GraphiteMiddleware",
]

ROOT_URLCONF = "django_workload.urls"

TEMPLATES = [
    {
        "BACKEND": "django.template.backends.django.DjangoTemplates",
        "DIRS": [],
        "APP_DIRS": True,
        "OPTIONS": {
            "context_processors": [
                "django.template.context_processors.debug",
                "django.template.context_processors.request",
                "django.contrib.auth.context_processors.auth",
                "django.contrib.messages.context_processors.messages",
            ],
        },
    },
]

WSGI_APPLICATION = "django_workload.wsgi.application"


# Database
# https://docs.djangoproject.com/en/1.11/ref/settings/#databases

DATABASES = {
    "default": {
        "ENGINE": "django_cassandra_engine",
        "NAME": "db",
        "TEST_NAME": "test_db",
        "HOST": "localhost",
        "OPTIONS": {
            "replication": {"strategy_class": "SimpleStrategy", "replication_factor": 1}
        },
    }
}


# Password validation
# https://docs.djangoproject.com/en/1.11/ref/settings/#auth-password-validators

AUTH_PASSWORD_VALIDATORS = [
    {
        "NAME": "django.contrib.auth.password_validation.UserAttributeSimilarityValidator",
    },
    {
        "NAME": "django.contrib.auth.password_validation.MinimumLengthValidator",
    },
    {
        "NAME": "django.contrib.auth.password_validation.CommonPasswordValidator",
    },
    {
        "NAME": "django.contrib.auth.password_validation.NumericPasswordValidator",
    },
]


# Internationalization
# https://docs.djangoproject.com/en/1.11/topics/i18n/

LANGUAGE_CODE = "en-us"

TIME_ZONE = "UTC"

USE_I18N = True

USE_L10N = True

USE_TZ = True


# Static files (CSS, JavaScript, Images)
# https://docs.djangoproject.com/en/1.11/howto/static-files/

STATIC_URL = "/static/"

# Statsd default configuration
# https://django-statsd.readthedocs.io/en/latest/
# http://statsd.readthedocs.io/en/latest/
STATSD_CLIENT = "django_statsd.clients.normal"
STATSD_HOST = "localhost"
STATSD_PORT = 8125
STATSD_PREFIX = None
STATSD_MAXUDPSIZE = 512
STATSD_IPV6 = False

# Cache configuration
CACHES = {
    "default": {
        "BACKEND": "django.core.cache.backends.memcached.PyLibMCCache",
        "LOCATION": "127.0.0.1:11811",
    }
}
