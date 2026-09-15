package jp.xenia.emulator;

import android.content.Context;

/** Labels follow the shared console enum indices; persisted values stay numeric. */
final class ProfileLabels {
    static String value(Context context, String field, String value, String fallback) {
        if (field.equals("live"))
            return context.getString(value.equals("true") ? R.string.state_on : R.string.state_off);
        int resource;
        switch (field) {
            case "country":
                resource = R.array.profile_countries;
                break;
            case "language":
                resource = R.array.profile_languages;
                break;
            case "subscription":
                resource = R.array.profile_subscriptions;
                break;
            case "zone":
                resource = R.array.profile_zones;
                break;
            default:
                return fallback;
        }
        try {
            int index = Integer.parseInt(value);
            String[] labels = context.getResources().getStringArray(resource);
            return index >= 0 && index < labels.length && !labels[index].isEmpty() ? labels[index]
                                                                                   : fallback;
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    private ProfileLabels() {}
}
