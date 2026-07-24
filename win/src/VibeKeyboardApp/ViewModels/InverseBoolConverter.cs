using System;
using System.Globalization;
using System.Windows.Data;

namespace VibeKeyboardApp.ViewModels
{
    /// <summary>
    /// Inverts a boolean value for data binding.
    /// </summary>
    public sealed class InverseBoolConverter : IValueConverter
    {
        public static readonly InverseBoolConverter Instance = new();

        public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
        {
            if (value is bool b)
                return !b;
            return true;
        }

        public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
        {
            if (value is bool b)
                return !b;
            return false;
        }
    }
}
