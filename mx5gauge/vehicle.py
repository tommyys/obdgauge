"""Vehicle identity: who is this car, and what can its gauges show?

Pure functions over a VIN string plus small lookup tables — no I/O, so it is
host-testable and ports to the firmware alongside `pids` and `metrics`.

Two jobs:

1. **Identify** the car. The VIN (mode 09, PID 02) reliably yields the
   *manufacturer* (the WMI — first three characters) and the *model year*
   (position 10). It does **not** yield a model name: going from VIN to
   "MX-5" requires a commercial VIN database, so the model is optional and
   comes from a hint table or the user's config. We show what we actually
   know and degrade honestly for the rest.

2. **Scale** the gauges. A dial that redlines at 7000 rpm is right for an
   MX-5 and wrong for a diesel. Per-make/model profiles supply the dial
   limits so the same firmware suits any car.
"""

# Characters a VIN may legally contain. I, O and Q are excluded by the
# standard precisely because they are confusable with 1 and 0.
VIN_ALPHABET = '0123456789ABCDEFGHJKLMNPRSTUVWXYZ'

# Model-year codes for position 10, in cycle order. The sequence repeats every
# 30 years, so a letter is ambiguous (S is both 1995 and 2025) — `model_year`
# resolves that by rejecting implausibly-future years.
_YEAR_LETTERS = 'ABCDEFGHJKLMNPRSTVWXY'      # A = 2010 ... Y = 2030
_YEAR_DIGITS = '123456789'                   # 1 = 2031 ... 9 = 2039

# World Manufacturer Identifier -> make. Best-effort and deliberately
# extensible: an unknown WMI degrades to showing the raw code rather than
# guessing. Add entries as cars turn up.
WMI = {
    # Mazda
    'JM0': 'Mazda', 'JM1': 'Mazda', 'JM3': 'Mazda', 'JM6': 'Mazda',
    'JM7': 'Mazda', 'JMZ': 'Mazda', '3MZ': 'Mazda', '4F2': 'Mazda',
    '4F4': 'Mazda',
    # Honda / Acura
    'JHM': 'Honda', 'JHL': 'Honda', 'JHG': 'Honda', '1HG': 'Honda',
    '2HG': 'Honda', '2HK': 'Honda', '5FN': 'Honda', '5J6': 'Honda',
    'SHH': 'Honda', 'MRH': 'Honda', '19X': 'Honda', '19U': 'Acura',
    # Toyota / Lexus
    'JTD': 'Toyota', 'JTE': 'Toyota', 'JTG': 'Toyota', 'JTK': 'Toyota',
    'JTL': 'Toyota', 'JTM': 'Toyota', 'JTN': 'Toyota', 'JT2': 'Toyota',
    'JT3': 'Toyota', 'JT4': 'Toyota', 'MR0': 'Toyota', '4T1': 'Toyota',
    '5TD': 'Toyota', 'JTH': 'Lexus', 'JTJ': 'Lexus', '2T2': 'Lexus',
    '58A': 'Lexus',
    # Nissan / Subaru / Mitsubishi / Suzuki / Isuzu
    'JN1': 'Nissan', 'JN6': 'Nissan', 'JN8': 'Nissan', 'VSK': 'Nissan',
    'JF1': 'Subaru', 'JF2': 'Subaru', '4S3': 'Subaru', '4S4': 'Subaru',
    'JA3': 'Mitsubishi', 'JA4': 'Mitsubishi', 'ML3': 'Mitsubishi',
    'MMB': 'Mitsubishi', 'JS2': 'Suzuki', 'JS3': 'Suzuki', 'MA3': 'Suzuki',
    'MPA': 'Isuzu', 'MP1': 'Isuzu', 'JAA': 'Isuzu',
    # Malaysia
    'PL1': 'Proton', 'PM2': 'Perodua',
    # Korea
    'KMH': 'Hyundai', 'KM8': 'Hyundai', 'KNA': 'Kia', 'KND': 'Kia',
    'KNE': 'Kia', 'KNM': 'Renault Samsung',
    # Europe
    'WVW': 'Volkswagen', 'WV1': 'Volkswagen', 'WV2': 'Volkswagen',
    'WVG': 'Volkswagen', 'WBA': 'BMW', 'WBS': 'BMW', 'WBY': 'BMW',
    'WMW': 'MINI', 'WDB': 'Mercedes-Benz', 'WDC': 'Mercedes-Benz',
    'WDD': 'Mercedes-Benz', 'W1K': 'Mercedes-Benz', 'W1N': 'Mercedes-Benz',
    'WAU': 'Audi', 'WA1': 'Audi', 'TRU': 'Audi', 'WP0': 'Porsche',
    'WP1': 'Porsche', 'TMB': 'Skoda', 'VF1': 'Renault', 'VF3': 'Peugeot',
    'VF7': 'Citroen', 'ZFA': 'Fiat', 'ZAR': 'Alfa Romeo', 'YV1': 'Volvo',
    'YV4': 'Volvo', 'LYV': 'Volvo', 'SAL': 'Land Rover', 'SAJ': 'Jaguar',
    'SCC': 'Lotus', 'VSS': 'SEAT',
    # North America
    '1G1': 'Chevrolet', '1GC': 'Chevrolet', '1FA': 'Ford', '1FM': 'Ford',
    '1FT': 'Ford', 'WF0': 'Ford', '3FA': 'Ford', '1C4': 'Chrysler',
    '1C6': 'Ram', '2C3': 'Chrysler', '1N4': 'Nissan', '5N1': 'Nissan',
    '1J4': 'Jeep', '1N6': 'Nissan',
    # China
    'LSV': 'SAIC Volkswagen', 'LFV': 'FAW-VW', 'LVS': 'Ford China',
    'LGX': 'BYD', 'LC0': 'BYD', 'LB3': 'Geely', 'LSJ': 'MG',
    'LVV': 'Chery', 'L6T': 'Zotye',
}

# Optional model names, keyed by the longest matching VIN prefix. VIN alone
# cannot give a model, so this is a hand-maintained shortcut for cars we
# actually meet. Extend as needed, or pass `model=` from config/CLI.
#
#   'JM0ND'  : 'MX-5',        # example: Mazda ND-chassis roadster
MODEL_HINTS = {}

# Dial limits per car, so the gauges suit the engine rather than assuming a
# 2.0 Skyactiv. Keys are 'Make' or 'Make Model' (the more specific wins).
# `power_max` is the kW top of the power dial.
PROFILES = {
    'Mazda MX-5':  {'rpm_max': 8000, 'rpm_red': 7000, 'power_max': 140},
    'Mazda':       {'rpm_max': 7000, 'rpm_red': 6200, 'power_max': 150},
    'Honda':       {'rpm_max': 7000, 'rpm_red': 6500, 'power_max': 150},
    'Honda Civic': {'rpm_max': 7000, 'rpm_red': 6500, 'power_max': 130},
    'Lexus':       {'rpm_max': 6500, 'rpm_red': 6000, 'power_max': 190},
    'Toyota':      {'rpm_max': 6500, 'rpm_red': 6000, 'power_max': 150},
    'Proton':      {'rpm_max': 7000, 'rpm_red': 6000, 'power_max': 130},
    'Perodua':     {'rpm_max': 7000, 'rpm_red': 6000, 'power_max': 100},
    'BMW':         {'rpm_max': 7000, 'rpm_red': 6500, 'power_max': 250},
    'Porsche':     {'rpm_max': 8000, 'rpm_red': 7200, 'power_max': 300},
}

# Used when we know nothing about the car. Wide enough not to peg the needle
# on anything ordinary.
DEFAULT_PROFILE = {'rpm_max': 8000, 'rpm_red': 6800, 'power_max': 160}

# Current year, used only to disambiguate the 30-year VIN year cycle. A
# constant (not `time`) keeps this module pure and its tests stable; bump it
# whenever, it only has to be within a couple of decades to work.
THIS_YEAR = 2026


def clean_vin(text):
    """Strip a VIN down to legal characters, upper-cased.

    Replies often carry padding nulls, spaces or a length prefix; anything
    outside the VIN alphabet is noise.
    """
    if not text:
        return ''
    return ''.join(c for c in text.upper() if c in VIN_ALPHABET)


def valid_vin(vin):
    """True for a plausible 17-character VIN.

    We do not verify the check digit: it is only mandatory in North America,
    so rejecting on it would throw away perfectly good Japanese-market VINs.
    """
    return len(vin or '') == 17


def model_year(vin, now_year=THIS_YEAR):
    """Model year from VIN position 10, or None.

    The code cycles every 30 years, so each letter has two readings (S =
    1995 or 2025). We take the recent one, stepping back 30 years at a time
    until the result is not in the future — `now_year + 1` is allowed
    because cars are sold ahead of their model year.
    """
    if not valid_vin(vin):
        return None
    c = vin[9]
    if c in _YEAR_LETTERS:
        year = 2010 + _YEAR_LETTERS.index(c)
    elif c in _YEAR_DIGITS:
        year = 2031 + _YEAR_DIGITS.index(c)
    else:
        return None
    while year > now_year + 1:
        year -= 30
    return year


def make_of(vin):
    """Manufacturer from the WMI, or None if we don't recognise it."""
    if not vin or len(vin) < 3:
        return None
    return WMI.get(vin[:3])


def model_hint(vin):
    """Model name for a known VIN prefix, or None. Longest match wins."""
    if not vin:
        return None
    best = None
    for prefix, name in MODEL_HINTS.items():
        if vin.startswith(prefix):
            if best is None or len(prefix) > len(best[0]):
                best = (prefix, name)
    return best[1] if best else None


def profile_for(make, model=None):
    """Dial limits for a car. 'Make Model' beats 'Make' beats the default."""
    if make and model:
        p = PROFILES.get('%s %s' % (make, model))
        if p:
            return dict(p)
    if make:
        p = PROFILES.get(make)
        if p:
            return dict(p)
    return dict(DEFAULT_PROFILE)


def identify(vin=None, make=None, model=None, year=None, source=None,
             now_year=THIS_YEAR):
    """Build the car-identity payload the UI banner renders.

    Explicit `make`/`model`/`year` arguments always win — they come from the
    user's config or a capture header, and the user knows their own car
    better than our WMI table does. Everything else is inferred from the VIN.
    """
    vin = clean_vin(vin)
    ok = valid_vin(vin)
    wmi = vin[:3] if ok else None

    if make is None and ok:
        make = make_of(vin)
    if model is None and ok:
        model = model_hint(vin)
    if year is None and ok:
        year = model_year(vin, now_year=now_year)

    # An unrecognised WMI is still information — show the code rather than
    # pretending we have no idea what car this is.
    if make:
        label = make.upper()
    elif wmi:
        label = 'WMI %s' % wmi
    else:
        label = 'OBD-II'
    if model:
        label += ' ' + model.upper()

    out = profile_for(make, model)
    out.update({
        'vin': vin if ok else None,
        'wmi': wmi,
        'make': make,
        'model': model,
        'year': year,
        'label': label,
        'source': source,
        'known': bool(make),
    })
    return out


# --- replay -----------------------------------------------------------------

# Car Scanner profile strings name the make, e.g. 'Mazda OBD-II / EOBD'.
_HEADER_MAKES = ('Mazda', 'Honda', 'Toyota', 'Lexus', 'Nissan', 'Subaru',
                 'Mitsubishi', 'Suzuki', 'Hyundai', 'Kia', 'Volkswagen',
                 'BMW', 'Mercedes-Benz', 'Audi', 'Ford', 'Proton', 'Perodua')


def from_capture_header(header):
    """Identify the car behind a .brc capture.

    Captures hold no VIN — Car Scanner never asks for one — so we fall back
    to the profile string it does record, and leave the year unknown rather
    than inventing one.
    """
    header = header or {}
    blob = ' '.join(str(header.get(k) or '')
                    for k in ('carprofile', 'car', 'profile'))
    make = None
    for cand in _HEADER_MAKES:
        if cand.lower() in blob.lower():
            make = cand
            break
    name = (header.get('car') or '').strip()
    # 'My car' is Car Scanner's default label — no information in it.
    model = name if name and name.lower() not in ('my car', 'car', '') else None
    return identify(make=make, model=model, source='capture')
