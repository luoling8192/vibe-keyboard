using System.Windows.Controls;
using VibeKeyboardApp.ViewModels;

namespace VibeKeyboardApp.Views
{
    public partial class DevicePage : UserControl
    {
        public DevicePage() { InitializeComponent(); }

        private void Reconnect_Click(object sender, System.Windows.RoutedEventArgs e)
        {
            if (DataContext is AppModel model)
                model.Reconnect();
        }
    }
}
